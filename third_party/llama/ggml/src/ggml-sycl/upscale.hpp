#pragma once

#include "common.hpp"
#include "dpct/helper.hpp"
#include <sycl/sycl.hpp>

void ggml_sycl_upscale(ggml_backend_sycl_context& ctx, ggml_tensor* dst);
