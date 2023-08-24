#include "Generators.h"
#include "Search_Cuda.h"
#include "beam_search_scorer_cuda.cuh"
#include "beam_search_scorer_cuda.h"
#include "beam_search_topk.h"
#include "cuda_common.h"
#include <queue>

namespace Generators {

void OnCudaError(cudaError_t error)
{
  printf("Cuda Error: %s\n", cudaGetErrorString(error));
  assert(false);
  throw std::exception();
}

void Launch_SoftMax(int32_t* next_tokens, const ScoreType* next_token_scores, int batch_size, int vocab_size, cudaStream_t stream);
void Launch_CheckForEOS(int32_t* next_tokens, int next_tokens_count, bool* eos_meet, int eos_token_id, int pad_token_id, bool* done_cpu, cudaStream_t stream);
void LaunchAddProbsKernel(ScoreType* log_probs, ScoreType* cum_log_probs, const int batch_size, const int num_beams, const int vocab_size, cudaStream_t stream);
void LaunchRepetitionPenaltyProcessor(const int32_t* sequences, ScoreType* next_token_scores, int batch_size, int num_beams, int vocab_size, int max_sequence_length, int current_sequence_length, ScoreType repetition_penalty, cudaStream_t stream);
void Launch_log_softmax(ScoreType* values, unsigned count, cudaStream_t stream);

Search_Cuda::Search_Cuda(SearchParams_Cuda& params)
    : params_{params},
      allocator_cpu_{Ort::Allocator::GetWithDefaultOptions()},
      allocator_cuda_{*params.p_allocator_cuda} {

  auto batch_beam_size = params.BatchBeamSize();

  int64_t sequences_dims[] = {batch_beam_size, params_.max_length};

  // below buffers are on cpu
  sequences_space_ = AllocateBuffer<int32_t>(&allocator_cpu_,
                                             sequences_space_buffer_,
                                             SafeInt<size_t>(2) * batch_beam_size * params_.max_length);
  memset(sequences_space_.data(), 0, sequences_space_.size_bytes());
  sequences_.Init(sequences_space_, static_cast<int>(batch_beam_size), params_.sequence_length, params_.max_length);

  sequence_lengths_ = AllocateBuffer<int32_t>(&allocator_cpu_, sequence_lengths_buffer_, batch_beam_size);
  eos_meet_ = AllocateBuffer<bool>(&allocator_cuda_, eos_meet_buffer_, batch_beam_size);
  cudaMemsetAsync(eos_meet_.data(), 0, eos_meet_.size_bytes(), params_.cuda_stream);

  // below buffers are on cpu or cuda
  size_t next_token_size = SafeInt<size_t>(batch_beam_size) * params_.vocab_size;
  next_token_scores_ = AllocateBuffer<ScoreType>(&allocator_cuda_, next_token_scores_buffer_, next_token_size);
  cudaMemsetAsync(next_token_scores_.data(), 0, next_token_scores_.size_bytes(), params_.cuda_stream);

  done_cpu_ = CudaMallocHostArray<bool>(1);

  SetInputSequence();
}

GreedySearch_Cuda::GreedySearch_Cuda(SearchParams_Cuda& params)
    : Search_Cuda{params} {
  next_tokens_ = AllocateBuffer<int32_t>(&allocator_cuda_, next_tokens_buffer_, SafeInt<size_t>(params.batch_size));
  cudaMemsetAsync(next_tokens_.data(), 0, next_tokens_.size_bytes(), params_.cuda_stream);

  next_tokens_cpu_ = CudaMallocHostArray<int32_t>(next_tokens_.size_bytes());
}

BeamSearch_Cuda::BeamSearch_Cuda(SearchParams_Cuda& params)
    : Search_Cuda{params} {
  assert(params_.num_beams > 1);  // If 1, use GreedySearch
  auto batch_beam_size = params_.BatchBeamSize();
  beam_scorer_ = std::make_unique<BeamSearchScorer_Cuda>(params_, allocator_cpu_, allocator_cuda_);

  size_t sequences_size = 2 * params_.BatchBeamSize()* params_.max_length;
  sequences_gpu_ = CudaMallocHostArray<int32_t>(sequences_size);
  sequences_.InitDevice(std::span<int32_t>{sequences_gpu_.get(), sequences_size});
  CudaCheck() == cudaMemcpyAsync(sequences_gpu_.get(), sequences_space_.data(), (sequences_size / 2)*sizeof(int32_t), cudaMemcpyHostToDevice, params_.cuda_stream);

  topk_next_tokens_ = CudaMallocArray<int32_t>(2 * batch_beam_size);
  topk_next_indices_ = CudaMallocArray<int32_t>(2* batch_beam_size);
  topk_next_scores_ = CudaMallocArray<ScoreType>(2 * batch_beam_size);

  constexpr size_t max_parts_of_vocab = 128;
  size_t topk_buffer_size = SafeInt<size_t>(batch_beam_size) * (max_parts_of_vocab + 1) * params_.num_beams * 2 * 2;
  topk_buffer_ = CudaMallocArray<ScoreType>(topk_buffer_size);
  static_assert(sizeof(ScoreType)==sizeof(int32_t)); // The topk_buffer assumes these match, fix for float16

  cudaMemsetAsync(topk_buffer_.get(), 0, topk_buffer_size*sizeof(ScoreType), params_.cuda_stream);
}

void Search_Cuda::SetInputSequence() {
  // The original inputs are not expanded, this expands them in place into the sequences
  std::span<int32_t> sequences_0 = sequences_space_;
  for (size_t batch = 0; batch < params_.batch_size; batch++) {
    for (size_t beam = 0; beam < params_.num_beams; beam++) {
      for (int j = 0; j < params_.sequence_length; j++) {
        sequences_0[(batch * params_.num_beams + beam) * params_.max_length + j] =
            static_cast<int32_t>(params_.input_ids[batch * params_.sequence_length + j]);
      }
    }
  }
}

void Search_Cuda::SetLogits(OrtValue& logits) {
  // Logits has shape (batch_size, input_length, vocab_size),
  // where input_length equals to parameters_->sequence_length for first subgraph call, and 1 for the remaining calls.
  auto logits_shape = logits.GetTensorTypeAndShapeInfo()->GetShape();
  assert(logits_shape.size() == 3);
  const ScoreType* logits_data = logits.GetTensorMutableData<ScoreType>();

  auto input_length = logits_shape[1];
  auto vocab_size = logits_shape[2];
  auto batch_beam_size = params_.BatchBeamSize();
  assert(vocab_size == params_.vocab_size);

  // Get logits for the last token:
  //    next_token_logits = logits[:, -1, :], and the result shape is (batch_size, vocab_size)
  // When input_length == 1, use logits directly in SoftmaxCPU below so it only need for input_length > 1.
  const ScoreType* current_logits = logits_data + (input_length - 1) * vocab_size;
  for (int i = 0; i < batch_beam_size; i++) {
    std::span<const ScoreType> source(current_logits, vocab_size);
    std::span<ScoreType> target = next_token_scores_.subspan(i * vocab_size, vocab_size);
    CudaCheck() == cudaMemcpyAsync(target.data(), source.data(), source.size_bytes(), cudaMemcpyDeviceToDevice, params_.cuda_stream);
    current_logits += input_length * vocab_size;

    Launch_log_softmax(target.data(), target.size(), params_.cuda_stream);
  }
}

#if 0
    if (do_sampling) {
      ORT_RETURN_IF_ERROR(SamplingCpuHelper::Sample(allocator,
                                                    thread_pool,
                                                    next_token_scores,
                                                    sampling_state,
                                                    greedy_state,
                                                    parameters,
                                                    dumper));
}
#endif

std::span<int32_t> GreedySearch_Cuda::GetNextTokens() {
  return next_tokens_;
}

std::span<int32_t> BeamSearch_Cuda::GetNextTokens() {
  return beam_scorer_->GetNextTokens();
}

std::span<int32_t> BeamSearch_Cuda::GetNextIndices() {
  return beam_scorer_->GetNextIndicesCPU();
}

int Search_Cuda::GetSequenceLength() {
  return sequences_.GetSequenceLength();
}

void BeamSearch_Cuda::NextTokensFromLogits() {
  auto beam_scores = beam_scorer_->GetNextScores();

  // Add beam score to next token scores. Corresponding python code is like:
  //    next_token_scores = next_token_scores + beam_scores[:, None].expand_as(next_token_scores)
  LaunchAddProbsKernel(next_token_scores_.data(), beam_scores.data(),
                       params_.batch_size, params_.num_beams, params_.vocab_size, params_.cuda_stream);

  // TODO: Write output scores?

  if (params_.num_beams <= 32) {
    constexpr size_t max_parts_of_vocab = 128;
    size_t candidate_count = SafeInt<size_t>(params_.BatchBeamSize()) * 2 * params_.num_beams;
    float* topk_tmp_buffer = topk_buffer_.get();
    float* topk_scores_1st_stage = topk_tmp_buffer;
    int32_t* topk_tokens_1st_stage = reinterpret_cast<int32_t*>(topk_scores_1st_stage + candidate_count * max_parts_of_vocab);
    float* topk_scores_2nd_stage = reinterpret_cast<float*>(topk_tokens_1st_stage + candidate_count * max_parts_of_vocab);
    int32_t* topk_tokens_2nd_stage = reinterpret_cast<int32_t*>(topk_scores_2nd_stage + candidate_count);

    cuda::BeamSearchTopK(next_token_scores_.data(),
                         params_.batch_size,
                         params_.num_beams,
                         params_.vocab_size,
                         2 * params_.num_beams,
                         topk_scores_1st_stage,
                         topk_tokens_1st_stage,
                         topk_scores_2nd_stage,
                         topk_tokens_2nd_stage,
                         topk_next_scores_.get(),
                         topk_next_tokens_.get(),
                         topk_next_indices_.get(),
                         params_.cuda_stream);
  }
  else
    assert(false);

  CudaCheck() == cudaStreamSynchronize(params_.cuda_stream);

  size_t size=params_.BatchBeamSize()*2;
  std::span<ScoreType> next_scores{topk_next_scores_.get(), size};
  std::span<int32_t> next_tokens{topk_next_tokens_.get(), size};
  std::span<int32_t> next_indices{topk_next_indices_.get(), size};

#if 0
  DumpCudaMemory("Next Scores", next_scores);
  DumpCudaMemory("Next Tokens", next_tokens);
  DumpCudaMemory("Next Indices", next_indices);
#endif

  beam_scorer_->Process(sequences_, next_scores, next_tokens, next_indices);
  next_tokens_ = beam_scorer_->GetNextTokens();
}

void GreedySearch_Cuda::NextTokensFromLogits() {
  auto next_token_scores = next_token_scores_.data();
  Launch_SoftMax(next_tokens_.data(), next_token_scores, params_.batch_size, params_.vocab_size, params_.cuda_stream);
}

void Search_Cuda::CheckForEOS() {
  assert(next_tokens_.size()==eos_meet_.size());
  Launch_CheckForEOS(next_tokens_.data(), next_tokens_.size(), eos_meet_.data(), params_.eos_token_id, params_.pad_token_id, done_cpu_.get(), params_.cuda_stream);
}

void GreedySearch_Cuda::AppendNextTokensToSequences() {
  CudaCheck()==cudaMemcpy(next_tokens_cpu_.get(), next_tokens_.data(), next_tokens_.size_bytes(), cudaMemcpyDeviceToHost);
  sequences_.AppendNextTokenToSequences(std::span<const int32_t>(next_tokens_cpu_.get(), next_tokens_.size()));

  if (sequences_.GetSequenceLength() == params_.max_length)
    *done_cpu_ = true;
}

bool BeamSearch_Cuda::IsDone() const
{
  beam_scorer_->IsDone();
  return beam_scorer_->IsDoneLater() || sequences_.GetSequenceLength() == params_.max_length;
}

void BeamSearch_Cuda::AppendNextTokensToSequences() {
  sequences_.AfterDeviceAppendedNextToken();
  //AppendNextTokenToSequences(beam_scorer_->GetNextIndicesCPU(), beam_scorer_->GetNextTokens());
  //if (sequences_.GetSequenceLength() == params_.max_length)
  //  *done_cpu_ = true;
}

void BeamSearch_Cuda::Finalize(size_t num_return_sequences, std::span<int32_t> output, std::span<float> sequence_scores) {
  beam_scorer_->Finalize(sequences_, num_return_sequences, output, sequence_scores);
}

#if 0
// Not needed, for greedy can just grab the output sequence directly?
void GreedySearch::Finalize(size_t num_return_sequences, std::span<int32_t> output, std::span<float> sequence_scores) {
  auto shape=output_sequences_->GetTensorTypeAndShapeInfo()->GetShape();
  size_t shape_count = std::accumulate(shape.begin(), shape.end(), 1LL, std::multiplies<int64_t>());

  // Copy the sequences to output
  std::span<int32_t> output{ output_sequences_->GetTensorMutableData<int32_t>(), shape_count};
  for (int batch_id = 0; batch_id < params_.batch_size; ++batch_id) {
    auto batch_output = output.subspan(
        static_cast<size_t>(batch_id) * params_.max_length,
        params_.max_length);
    std::span<const int32_t> sequence_source = sequences_.GetSequence(batch_id);
    std::copy(sequence_source, batch_output);
  }
}
#endif

std::span<ScoreType> Search_Cuda::GetScores(int batch_beam_index) {
  assert(batch_beam_index >= 0 && batch_beam_index < params_.BatchBeamSize());
  return next_token_scores_.subspan(batch_beam_index * params_.vocab_size, params_.vocab_size);
}

std::span<ScoreType> Search_Cuda::GetScores() {
  return next_token_scores_;
}

namespace Processors_Cuda {

void MinLength(Search_Cuda& search, int min_length) {
  if (search.sequences_.GetSequenceLength() >= min_length)
    return;

  const int batch_beam_size = search.params_.BatchBeamSize();
  for (int i = 0; i < batch_beam_size; i++) {
    std::span<ScoreType> beam_token_scores = search.GetScores(i);
    beam_token_scores[search.params_.eos_token_id] = std::numeric_limits<ScoreType>::lowest();
  }
}

void RepetitionPenalty(Search_Cuda& search, ScoreType penalty) {
  LaunchRepetitionPenaltyProcessor(search.GetSequences().GetCurrentDeviceSequences().data(),
    search.GetScores().data(), search.params_.batch_size, search.params_.num_beams, search.params_.vocab_size, 
    search.params_.max_length, search.GetSequenceLength(), penalty, search.params_.cuda_stream);
}

}  // namespace Processors

}  // namespace Generators