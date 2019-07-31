#pragma once
#include "..//transform.h"

namespace nncase
{
namespace transforms
{
    namespace k210
    {
        class strided_slice_motion_transform : public transform
        {
        public:
            void process(transform_context &context) override;

        protected:
            bool on_try_match(ir::node &node, transform_context &context) override;
        };
    }
}
}
