// Copyright (C) 2023-2024 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "text_callback_streamer.hpp"
#include "continuous_batching_impl.hpp"
#include "paged_attention_transformations.hpp"

namespace ov::genai {
template<class... Ts> struct overloaded : Ts... {using Ts::operator()...;};
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

ContinuousBatchingPipeline::ContinuousBatchingImpl::ContinuousBatchingImpl(
    const std::string& models_path,
    const Tokenizer& tokenizer,
    const SchedulerConfig& scheduler_config,
    const std::string& device,
    const ov::AnyMap& plugin_config) {
    m_tokenizer = tokenizer;

    ov::Core core;
    // The model can be compiled for GPU as well
    std::shared_ptr<ov::Model> model = core.read_model(models_path + "/openvino_model.xml");
    DeviceConfig device_config(core, scheduler_config, device, plugin_config);

    bool is_need_per_layer_cache_control = scheduler_config.use_cache_eviction;
    apply_paged_attention_transformations(model, device_config, is_need_per_layer_cache_control);
    init(model, scheduler_config, plugin_config,  device_config, core);
}

ContinuousBatchingPipeline::ContinuousBatchingImpl::ContinuousBatchingImpl(
    ov::Core& core,
    const std::shared_ptr<ov::Model>& model,
    const Tokenizer& tokenizer,
    const DeviceConfig& device_config,
    const SchedulerConfig& scheduler_config,
    const std::string& device,
    const ov::AnyMap& plugin_config,
    bool is_validation_mode_enabled) {
    m_tokenizer = tokenizer;
    m_is_validation_mode_enabled = is_validation_mode_enabled;
    init(model, scheduler_config, plugin_config,  device_config, core);
}

void ContinuousBatchingPipeline::ContinuousBatchingImpl::_pull_awaiting_requests() {
    std::lock_guard<std::mutex> lock{m_awaiting_requests_mutex};
    m_requests.insert(m_requests.end(), m_awaiting_requests.begin(), m_awaiting_requests.end());
    m_awaiting_requests.clear();
    for (const auto& request : m_requests) {
        request->pause_generation(false);
    }
}

GenerationHandle
ContinuousBatchingPipeline::ContinuousBatchingImpl::add_request(uint64_t request_id,
                                                               const ov::Tensor& input_ids,
                                                               ov::genai::GenerationConfig sampling_params) {
    sampling_params.set_eos_token_id(m_tokenizer.get_eos_token_id());
    sampling_params.validate();
    SequenceGroup::Ptr sequence_group = std::make_shared<SequenceGroup>(request_id, input_ids,
                                                                        sampling_params, 
                                                                        m_scheduler->get_config().block_size,
                                                                        m_scheduler->get_config().enable_prefix_caching);
    sequence_group->set_sequence_group_ptr(sequence_group);
    if (m_scheduler->get_config().enable_prefix_caching) {
        m_scheduler->restore_cached_blocks(sequence_group);
    }

    {
        std::lock_guard<std::mutex> lock{m_awaiting_requests_mutex};
        m_awaiting_requests.push_back(sequence_group);
    }
    return std::make_shared<GenerationHandleImpl>(sequence_group->get_generation_stream(), sampling_params);
};

GenerationHandle
ContinuousBatchingPipeline::ContinuousBatchingImpl::add_request(uint64_t request_id,
                                                                const std::string& prompt,
                                                                ov::genai::GenerationConfig sampling_params) {                           
    static ManualTimer timer("tokenize");
    timer.start();
    ov::Tensor input_ids = m_tokenizer.encode(prompt).input_ids;
    timer.end();
    return add_request(request_id, input_ids, sampling_params);
}

bool ContinuousBatchingPipeline::ContinuousBatchingImpl::has_non_finished_requests() {
    std::lock_guard<std::mutex> lock{m_awaiting_requests_mutex};
    return !m_awaiting_requests.empty() || !m_requests.empty();
}

void ContinuousBatchingPipeline::ContinuousBatchingImpl::step() {
    static ManualTimer step_timer("step()");
    step_timer.start();

    // Pull awaiting requests
    _pull_awaiting_requests();

    m_pipeline_metrics.requests = m_requests.size();

    size_t iteration_number = 0;
    // cycle to generate several tokens per one iteration, e.g. for speculative decoding case
    bool to_generate = true;
    while (to_generate) {
        Scheduler::Output scheduler_output;
        {
            static ManualTimer timer("scheduling");
            timer.start();
            scheduler_output = m_scheduler->schedule(m_requests);
            m_pipeline_metrics.scheduled_requests = scheduler_output.m_scheduled_sequence_groups_ids.size();
            m_pipeline_metrics.cache_usage = scheduler_output.m_cache_usage;
            m_pipeline_metrics.max_cache_usage =
                std::max(m_pipeline_metrics.max_cache_usage, scheduler_output.m_cache_usage);
            _register_step_cache_usage(scheduler_output.m_cache_usage);
            m_pipeline_metrics.avg_cache_usage = _get_current_running_average_cache_usage();
            m_cache_manager->copy_blocks(scheduler_output.m_block_copy_map);
            timer.end();
        }

        // if no tokens were scheduled, we are out of memory
        if (scheduler_output.m_total_num_scheduled_tokens == 0) {
            for (size_t i = 0; i < m_requests.size(); ++i) {
                SequenceGroup::Ptr sequence_group = m_requests[i];
                sequence_group->set_out_of_memory();
                sequence_group->notify_handle();
            }
            _free_non_running_requests();
            return;
        }

        ov::Tensor logits;
        {
            static ManualTimer timer("forward");
            timer.start();
            logits = m_model_runner->forward(m_requests, scheduler_output);
            timer.end();

            ov::InferRequest infer_request = m_model_runner->get_infer_request();
            ov::CompiledModel compiled_model = infer_request.get_compiled_model();
            const bool is_profiling_enabled = compiled_model.get_property(ov::enable_profiling);

            // collect detailed statistic
            if (is_profiling_enabled) {
                std::vector<ov::ProfilingInfo> profiling_info = m_model_runner->get_infer_request().get_profiling_info();
                for (const ov::ProfilingInfo& info : profiling_info) {
                    double current_time = info.real_time.count();
                    if (info.node_type == "PagedAttentionExtension") {
                        m_perf.m_paged_attention_time_ms += current_time;
                    } else if (info.node_type == "FullyConnected") {
                        m_perf.m_matmul_time_ms += current_time;
                    }
                    m_perf.m_infer_total_ms += current_time;
                }
            }
        }

    #ifdef DEBUG_CACHE_STATE_DUMP

        CacheStateDumper dumper(CacheStateDumper::get_run_id_for_generation_step(step_count, "before_eviction"));
        dumper.dump_cache_state(*m_scheduler, m_requests, step_count);
    #endif
        const auto& sched_config = m_scheduler->get_config();

        // evict unimportant blocks from KV cache, if requested
        if (sched_config.use_cache_eviction) {
            maybe_evict_cache_blocks(sched_config);
        }

    #ifdef DEBUG_CACHE_STATE_DUMP
        CacheStateDumper dumper_after(CacheStateDumper::get_run_id_for_generation_step(step_count, "eviction"));
        dumper_after.dump_cache_state(*m_scheduler, m_requests, step_count);
        step_count++;
    #endif

        SamplerOutput sampler_output;
        {
            static ManualTimer timer("sample");
            timer.start();
            sampler_output = m_sampler->sample(m_requests, logits, m_is_validation_mode_enabled);
            timer.end();
        }

        // process sampler_output (e.g. fork or drop sequences from BlockScheduler)
        {
            static ManualTimer timer("fork / free sequence");
            timer.start();

            for (const auto& pair : sampler_output.m_forked_sequences) {
                uint64_t parent_id = pair.first;
                const std::list<uint64_t>& child_ids = pair.second;
                for (auto& child_id : child_ids)
                    m_scheduler->fork_sequence(parent_id, child_id);
            }

            for (auto seq_id : sampler_output.m_dropped_sequences)
                m_scheduler->free_sequence(seq_id);

            timer.end();
        }

        // notify requests dropped by handle
        {
            static ManualTimer timer("notify requests dropped by handle");
            timer.start();
            _notify_requests_dropped_by_handle();
            timer.end();
        }

        to_generate = false;
        for (auto& request : m_requests) {
            const auto& sampling_params = request->get_sampling_parameters();
            if (!sampling_params.is_speculative_decoding()) {
                to_generate = false;
                break;
            }
            if (sampling_params.num_assistant_tokens_schedule == NumAssistatantTokensScheduleType::CONSTANT &&
                sampling_params.num_assistant_tokens <= iteration_number) {
                request->pause_generation(true);
            }
            to_generate |= request->can_generate_tokens();
        }
        iteration_number += 1;
    }

    // free non running requests for current step

    {
        static ManualTimer timer("free non running requests");
        timer.start();
        _free_non_running_requests();
        timer.end();
    }

    step_timer.end();
}

std::vector<EncodedGenerationResult>
ContinuousBatchingPipeline::ContinuousBatchingImpl::generate(const std::vector<ov::Tensor>& input_ids,
                                                             const std::vector<GenerationConfig>& sampling_params,
                                                             const StreamerVariant& streamer) {
    OPENVINO_ASSERT(!has_non_finished_requests(), "Generate cannot be called while ContinuousBatchingPipeline is already in running state. Use ContinuousBatchingPipeline::add_request");
    OPENVINO_ASSERT(input_ids.size() == sampling_params.size());
    const std::shared_ptr<StreamerBase>& streamer_ptr = std::visit(overloaded{
        [](std::monostate) -> std::shared_ptr<StreamerBase> {
            return nullptr;
        },
        [](const std::shared_ptr<StreamerBase>& streamer) {
            return streamer;
        },
        [this](const std::function<bool(std::string)>& streamer) -> std::shared_ptr<StreamerBase> {
            return std::make_unique<TextCallbackStreamer>(m_tokenizer, streamer);
        }
    }, streamer);

    std::vector<GenerationHandle> generations;
    for (size_t request_id = 0; request_id < input_ids.size(); ++request_id) {
        OPENVINO_ASSERT(1 == input_ids[request_id].get_shape().at(0), "Use multiple tensors to pass a batch.");
        generations.push_back(add_request(request_id, input_ids[request_id], sampling_params[request_id]));
    }

    std::vector<EncodedGenerationResult> results;
    results.reserve(m_awaiting_requests.size());

    bool continue_generation = true;
    while (has_non_finished_requests() && continue_generation) {
        step();
        if (streamer_ptr) {
            std::unordered_map<uint64_t, GenerationOutput> token = generations.at(0).get()->back();
            OPENVINO_ASSERT(1 == token.size());
            OPENVINO_ASSERT(1 == token.begin()->second.generated_ids.size());
            continue_generation = !streamer_ptr->put(token.begin()->second.generated_ids.at(0));
        }
    }
    if (streamer_ptr) {
        streamer_ptr->end();
    }

    for (size_t generation_idx = 0; generation_idx < generations.size(); ++generation_idx) {
        const auto& generation = generations[generation_idx];
        EncodedGenerationResult result;
        result.m_request_id = 1;
        std::vector<GenerationOutput> generation_outputs = generation->read_all();
        std::sort(generation_outputs.begin(), generation_outputs.end(), [=] (GenerationOutput& r1, GenerationOutput& r2) {
            return r1.score > r2.score;
        });

        auto num_outputs = std::min(sampling_params[generation_idx].num_return_sequences, generation_outputs.size());
        for (size_t generation_output_idx = 0; generation_output_idx < num_outputs; ++generation_output_idx) {
            const auto& generation_output = generation_outputs[generation_output_idx];
            result.m_generation_ids.push_back(std::move(generation_output.generated_ids));
            result.m_scores.push_back(generation_output.score);
        }
        result.m_status = generation->get_status();
        results.push_back(std::move(result));
    }

    OPENVINO_ASSERT(results.size() == input_ids.size());
    return results;
}

void ContinuousBatchingPipeline::ContinuousBatchingImpl::_free_non_running_requests() {
    std::vector<SequenceGroup::Ptr>::iterator requests_iterator = m_requests.begin();
    while (requests_iterator != m_requests.end()) {
        const auto& request = *requests_iterator;
        if(request->has_finished() || request->out_of_memory() || request->handle_dropped()) {
            for (const auto& sequence: request->get_sequences()) {
                if (m_scheduler->has_block_table(sequence->get_id())) {
                    m_scheduler->free_sequence(sequence->get_id());
                }
            }
            m_sampler->clear_beam_search_info(request->get_request_id());
            requests_iterator = m_requests.erase(requests_iterator);
        } else {
            requests_iterator++;
        }
    }
}

void ContinuousBatchingPipeline::ContinuousBatchingImpl::_notify_requests_dropped_by_handle() {
    // Notify the last time by pushing empty output
    // This causes read() to unblock by adding anything to the queue
    for (SequenceGroup::Ptr& request : m_requests) {
        if (request->handle_dropped())
            request->push_empty_outputs();
    }
}

void ContinuousBatchingPipeline::ContinuousBatchingImpl::_register_step_cache_usage(float step_cache_usage) {
    if (m_previous_step_cache_usages.size() >= AVG_CACHE_USAGE_WINDOW_SIZE_IN_STEPS) {
        m_previous_step_cache_usages.pop_front();
    }
    m_previous_step_cache_usages.push_back(step_cache_usage);
}

float ContinuousBatchingPipeline::ContinuousBatchingImpl::_get_current_running_average_cache_usage() const {
    return std::accumulate(m_previous_step_cache_usages.begin(), m_previous_step_cache_usages.end(), 0.0) / m_previous_step_cache_usages.size();
}

void ContinuousBatchingPipeline::ContinuousBatchingImpl::maybe_evict_cache_blocks(const SchedulerConfig& sched_config) {
    std::unordered_map<SequenceGroup::Ptr, size_t> seq_group_to_num_blocks_evicted_map;
    auto sequence_attention_scores = m_model_runner->get_last_attention_scores();
    for (auto& seq_id_and_attention_scores : sequence_attention_scores) {
        auto seq_id = seq_id_and_attention_scores.first;
        const auto& attention_scores_for_all_decoder_layers = seq_id_and_attention_scores.second;
        if (m_seq_group_id_to_cache_eviction_algo_map.find(seq_id) == m_seq_group_id_to_cache_eviction_algo_map.end()) {
            auto num_decoder_layers = attention_scores_for_all_decoder_layers.size();

            m_seq_group_id_to_cache_eviction_algo_map[seq_id] = CacheEvictionAlgorithm(sched_config.cache_eviction_config, sched_config.block_size, num_decoder_layers);
        }
        auto& cache_eviction_algo = m_seq_group_id_to_cache_eviction_algo_map[seq_id];

        cache_eviction_algo.register_new_token_scores(attention_scores_for_all_decoder_layers);
        auto logical_blocks_to_evict = cache_eviction_algo.evict_logical_blocks();

        m_scheduler->free_blocks_from_sequence(seq_id, logical_blocks_to_evict);

        auto seq_group_ptr_it = std::find_if(m_requests.begin(), m_requests.end(), [seq_id](const SequenceGroup::Ptr& val) { return val->has_sequence_with_id(seq_id); });
        OPENVINO_ASSERT(seq_group_ptr_it != m_requests.end(), "could not find sequence group with sequence ", seq_id);
        auto seq_group_ptr = *seq_group_ptr_it;
        size_t num_blocks_evicted = logical_blocks_to_evict[0].size();

        if (seq_group_to_num_blocks_evicted_map.find(seq_group_ptr) != seq_group_to_num_blocks_evicted_map.end()) {
            OPENVINO_ASSERT(seq_group_to_num_blocks_evicted_map[seq_group_ptr] == num_blocks_evicted, "internal error - each sequence in the same group must have the same number of blocks evicted");
        } else {
            seq_group_to_num_blocks_evicted_map[seq_group_ptr] = num_blocks_evicted;
        }

    }
    for (const auto& seq_group_ptr_and_num_blocks_evicted : seq_group_to_num_blocks_evicted_map) {
        // Assuming that the evicted blocks are always full (since they by design are only selected from intermediate-age blocks)
        auto seq_group_ptr = seq_group_ptr_and_num_blocks_evicted.first;
        auto num_blocks_evicted = seq_group_ptr_and_num_blocks_evicted.second;
        seq_group_ptr->register_token_eviction(num_blocks_evicted * sched_config.block_size);
    }
}

void ContinuousBatchingPipeline::ContinuousBatchingImpl::finish_request(int64_t request_id) {
    if (request_id == -1) {
        while (!m_requests.empty()) {
            const auto& request = *m_requests.rbegin();
            for (const auto& sequence : request->get_sequences()) {
                m_scheduler->free_sequence(sequence->get_id());
            }
            m_sampler->clear_beam_search_info(request->get_request_id());
            m_requests.pop_back();
        }
    } else {
        for (size_t i = 0; i < m_requests.size(); ++i) {
            auto& request = m_requests[i];
            if (request->get_request_id() != request_id) {
                continue;
            }
            for (const auto& sequence : request->get_sequences()) {
                m_scheduler->free_sequence(sequence->get_id());
            }
            m_sampler->clear_beam_search_info(request->get_request_id());
            m_requests.erase(m_requests.begin() + i);
            break;
        }
    }
}

std::vector<ContinuousBatchingPipeline::ContinuousBatchingImpl::GeneratedSequence>
ContinuousBatchingPipeline::ContinuousBatchingImpl::get_generated_sequences() {
    _pull_awaiting_requests();
    std::vector<ContinuousBatchingPipeline::ContinuousBatchingImpl::GeneratedSequence> result;
    for (const auto& request : m_requests) {
        const auto request_id = request->get_request_id();
        for (const auto& sequence : request->get_sequences()) {
            auto generated_ids = sequence->get_generated_ids();
            auto log_probs = sequence->get_generated_log_probs();
            result.emplace_back(request_id, sequence->get_grouped_id(), generated_ids, log_probs);
        }
    }
    return result;
}

ContinuousBatchingPipeline::ContinuousBatchingImpl::UpdateSeqResult
ContinuousBatchingPipeline::ContinuousBatchingImpl::update_generated_sequence(
    const ContinuousBatchingPipeline::ContinuousBatchingImpl::GeneratedSequence& candidate_sequence) {
    _pull_awaiting_requests();

    bool is_empty_generated_tokens = false;
    for (auto& request : m_requests) {
        if (candidate_sequence.request_id == request->get_request_id()) {
            bool is_seq_exists = false;
            // todo: iefode: multiseq
            size_t to_remove_tokens = 0, to_insert_tokens = 0;
            for (auto& sequence : request->get_sequences()) {
                if (candidate_sequence.sequence_id == sequence->get_grouped_id()) {
                    is_seq_exists = true;
                    auto present_ids = sequence->get_generated_ids();
                    const auto& candidate_ids = candidate_sequence.token_ids;

                    // remove extra tokens from sequence
                    {
                        auto token_idx = std::min(present_ids.size(), candidate_ids.size());
                        if (token_idx) {
                            while (token_idx-- > 0) {
                                if (present_ids[token_idx] == candidate_ids[token_idx]) {
                                    break;
                                }
                            }
                            to_remove_tokens = present_ids.size() - (token_idx + 1);
                            if (to_remove_tokens > 0) {
                                const auto gen_ids_before = sequence->get_generated_ids();
                                sequence->remove_last_tokens(to_remove_tokens);
                                present_ids = sequence->get_generated_ids();
                                const size_t gen_len_before = gen_ids_before.size(),
                                            gen_len_after = present_ids.size();
                                if (gen_len_after == 0) {
                                    is_empty_generated_tokens = true;
                                }
                                OPENVINO_ASSERT(gen_len_after < gen_len_before);
                                for (size_t i = gen_len_after; i < gen_len_before; ++i) {
                                    // todo
                                    // m_sampler->update_logit_processor(request->get_request_id(), gen_ids_before[i]);
                                }
                            }
                        }
                    }
                    // insert new tokens to sequence
                    {
                        OPENVINO_ASSERT(candidate_ids.size() >= present_ids.size());
                        const auto& candidate_log_probs = candidate_sequence.log_probs;
                        const size_t start_id = std::min(present_ids.size(), candidate_ids.size()),
                                        stop_id = std::max(present_ids.size(), candidate_ids.size());
                        to_insert_tokens = stop_id - start_id;
                        for (size_t i = start_id; i < stop_id; ++i) {
                            sequence->append_token(candidate_ids[i],  i < candidate_log_probs.size() ? candidate_log_probs[i] : 0.f);
                        }
                    }
                }
                break;
            }
            if (!is_seq_exists) {
                Sequence::Ptr new_sequence(new Sequence(candidate_sequence.sequence_id));
                const auto& generated_tokens = candidate_sequence.token_ids;
                const auto& generated_log_probs = candidate_sequence.log_probs;
                for (size_t i = 0; i < generated_tokens.size(); ++i) {
                    new_sequence->append_token(generated_tokens[i], generated_log_probs[i]);
                }
                request->add_sequence(new_sequence);
            }
            if (!is_empty_generated_tokens) {
                if (to_remove_tokens > 0) {
                    // request->decrease_processed_tokens(to_remove_tokens);
                }
                // to validate tokens/extend kv-cache before generation
                // request->set_validation_len(to_insert_tokens);
            } else if (to_remove_tokens > 0) {
                request->update_processed_tokens_num(request->get_prompt_len());
            }
            return ContinuousBatchingPipeline::ContinuousBatchingImpl::UpdateSeqResult(to_insert_tokens, to_remove_tokens);
        }
    }
    return {0, 0};
}
}
