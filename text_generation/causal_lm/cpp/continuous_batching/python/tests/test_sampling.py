import pytest
from typing import List, Tuple

from transformers import AutoTokenizer, AutoModelForCausalLM
from transformers import GenerationConfig as HFGenerationConfig
from optimum.intel import OVModelForCausalLM

from py_continuous_batching import ContinuousBatchingPipeline, GenerationConfig, SchedulerConfig, GenerationResult

def get_greedy() -> GenerationConfig:
    generation_config = GenerationConfig()
    generation_config.num_return_sequences = 1
    return generation_config

def get_beam_search() -> GenerationConfig:
    generation_config = GenerationConfig()
    generation_config.num_groups = 3
    generation_config.group_size = 2
    generation_config.max_new_tokens = 30
    generation_config.num_return_sequences = generation_config.num_groups * generation_config.group_size
    return generation_config

def get_test_dataset() -> Tuple[List[str], List[GenerationConfig]]:
    prompts = [
        "What is OpenVINO?",
        "How are you?",
        "What is your name?",
        "Tell me something about Canada"
    ]
    generation_configs = [
        get_greedy(),
        get_beam_search(),
        get_greedy(),
        get_beam_search()
    ]
    return (prompts, generation_configs)

def get_scheduler_config() -> SchedulerConfig:
    scheduler_config = SchedulerConfig()
    scheduler_config.dynamic_split_fuse = True
    scheduler_config.num_kv_blocks = 300
    # vLLM specific
    scheduler_config.max_num_batched_tokens = 256
    scheduler_config.max_num_seqs = 256

    return scheduler_config

def convert_to_hf(
    generation_config : GenerationConfig
) -> HFGenerationConfig:
    kwargs = {}

    # generic parameters
    kwargs['max_length'] = generation_config.max_length
    kwargs['max_new_tokens'] = generation_config.max_new_tokens

    if generation_config.num_groups * generation_config.group_size > 1:
        # beam search case
        kwargs['num_beam_groups'] = generation_config.num_groups
        kwargs['num_beams'] = generation_config.num_groups * generation_config.group_size
        kwargs['diversity_penalty'] = generation_config.diversity_penalty
        kwargs['repetition_penalty'] = generation_config.repetition_penalty
        kwargs['length_penalty'] = generation_config.length_penalty
        kwargs['no_repeat_ngram_size'] = generation_config.no_repeat_ngram_size
        kwargs['num_return_sequences'] = generation_config.num_return_sequences
        kwargs['output_scores'] = True
    elif generation_config.do_sample:
        # mulitinomial
        kwargs['temperature'] = generation_config.temperature
        kwargs['top_k'] = generation_config.top_k
        kwargs['top_p'] = generation_config.top_p
        kwargs['do_sample'] = generation_config.do_sample
    else:
        # greedy
        pass

    hf_generation_config = HFGenerationConfig(**kwargs)
    return hf_generation_config

def run_hugging_face(
    model_id : str,
    prompts: List[str],
    generation_configs: List[GenerationConfig],
    use_optimum: bool
) -> List[GenerationResult]:
    tokenizer = AutoTokenizer.from_pretrained(model_id)
    model = OVModelForCausalLM.from_pretrained(model_id, export=True) if use_optimum else \
            AutoModelForCausalLM.from_pretrained(model_id)
    generation_results: List[GenerationResult] = []

    for prompt, generation_config in zip(prompts, generation_configs):
        inputs = tokenizer(prompt, return_tensors="pt")
        prompt_len = len(inputs['input_ids'][0])
        generate_outputs = model.generate(**inputs, generation_config=convert_to_hf(generation_config), return_dict_in_generate=True)
        all_text_batch = tokenizer.batch_decode([generated_ids[prompt_len:] for generated_ids in generate_outputs.sequences], skip_special_tokens=True)

        generation_result = GenerationResult()
        generation_result.m_generation_ids = all_text_batch
        # sequences_scores are available only for beam search case
        if generation_config.is_beam_search:
            generation_result.m_scores = [score for score in generate_outputs.sequences_scores]
        generation_results.append(generation_result)

    return generation_results

def run_continuous_batching(
    model_path : str,
    scheduler_config : SchedulerConfig,
    prompts: List[str],
    generation_configs : List[GenerationConfig]
) -> List[GenerationResult]:
    pipe = ContinuousBatchingPipeline(model_path, scheduler_config)
    return pipe.generate(prompts, generation_configs)

# export models via
# optimum-cli export openvino -m meta-llama/Llama-2-7b-chat-hf llama2
# optimum-cli export openvino -m meta-llama/Llama-2-7b-chat-hf --fp16 llama2-fp16

# tested models:
# - facebook/opt-125m (opt125)
# - meta-llama/Llama-2-7b-chat-hf (llama2 or llama2-fp16)

def test_check_greedy_search():
    prompts, generation_configs = get_test_dataset()
    hf_results : List[GenerationResult] = run_hugging_face(model_id="facebook/opt-125m", prompts=prompts, generation_configs=generation_configs, use_optimum=True)
    my_results : List[GenerationResult] = run_continuous_batching("/home/sandye51/Documents/Programming/git_repo/openvino.genai/build/opt125", get_scheduler_config(), prompts, generation_configs)

    assert len(prompts) == len(hf_results)
    assert len(prompts) == len(my_results)

    for prompt, hf_result, my_result, generation_config in zip(prompts, hf_results, my_results, generation_configs):
        print(f"Prompt = {prompt}\nHF result = {hf_result}\nmy result = {my_result}")

        if generation_config.is_beam_search:
            assert len(hf_result.m_scores) == len(my_result.m_scores)
            for hf_score, my_score in zip(hf_result.m_scores, my_result.m_scores):
                # Note, that for fp32 / fp16 models scores are different less than 0.001
                assert abs(hf_score - my_score) < 0.02

        assert len(hf_result.m_generation_ids) == len(my_result.m_generation_ids)
        for hf_text, my_text in zip(hf_result.m_generation_ids, my_result.m_generation_ids):
            assert hf_text == my_text
