#include "../tflite_importer.h"
#include <ir/ops/resize_image.h>

using namespace nncase;
using namespace nncase::importer;
using namespace nncase::ir;

DEFINE_TFLITE_LOWER(RESIZE_BILINEAR)
{
    convert_resize_image(op, image_resize_bilinear);
}

void tflite_importer::convert_resize_image(const tflite::Operator &op, image_resize_mode_t mode)
{
    auto &input = get_tensor(op.inputs(), 0);
    auto &options = *op.builtin_options_as_ResizeBilinearOptions();
    auto new_size_tensor = load_tensor<int32_t, 1>(get_tensor(op.inputs(), 1));
    std::array<int32_t, 2> new_size { new_size_tensor[0], new_size_tensor[1] };

    auto pre_trans = nhwc_to_nchw(dt_float32, get_shape(*input.shape()));
    auto node = graph_.emplace<resize_image>(to_data_type(input.type()), mode, pre_trans->output().shape(), new_size, options.align_corners());
    auto sur_trans = nchw_to_nhwc(dt_float32, node->output().shape());
    node->input().connect(pre_trans->output());
    sur_trans->input().connect(node->output());

    input_tensors_.emplace(&pre_trans->input(), op.inputs()->Get(0));
    output_tensors_.emplace(op.outputs()->Get(0), &sur_trans->output());
}