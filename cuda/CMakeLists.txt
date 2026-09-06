add_executable(tail_cuda_proof tail_cuda_proof.cu)
target_link_libraries(tail_cuda_proof PRIVATE TailGovernor::tailgovernor CUDA::cudart)
set_target_properties(tail_cuda_proof PROPERTIES
    CUDA_ARCHITECTURES 120
    CUDA_STANDARD 20
    CUDA_STANDARD_REQUIRED ON)
add_test(NAME tg_cuda_proof COMMAND tail_cuda_proof)
