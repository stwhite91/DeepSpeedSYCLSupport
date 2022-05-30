import torch
from deepspeed.pt.deepspeed_linear import LinearModuleForZeroStage3
from deepspeed.pt.log_utils import logger
import deepspeed
from deepspeed.accelerator import literal_device
from deepspeed.accelerator import runtime as accel_runtime


def see_memory_usage(message):

    # Print message except when distributed but not rank 0
    logger.info(message)
    logger.info(
        "Memory Allocated %s GigaBytes ",
        accel_runtime.memory_allocated() / (1024 * 1024 * 1024),
    )
    logger.info(
        "Max Memory Allocated %s GigaBytes",
        accel_runtime.max_memory_allocated() / (1024 * 1024 * 1024),
    )
    logger.info(
        "Cache Allocated %s GigaBytes",
        accel_runtime.memory_cached() / (1024 * 1024 * 1024),
    )
    logger.info(
        "Max cache Allocated %s GigaBytes",
        accel_runtime.max_memory_cached() / (1024 * 1024 * 1024),
    )


tens = torch.rand(1024, 16384, dtype=torch.half, device=torch.device(literal_device()))
tens_back = tens.detach().clone()

#linear_bk = torch.nn.functional.linear
#torch.nn.functional.linear = deepspeed.pt.deepspeed_linear.LinearFunctionForZeroStage3.apply
model = LinearModuleForZeroStage3(16384, 16384)

model.to(literal_device()).half()

see_memory_usage("Before forward")
y = model(tens)

see_memory_usage("After forward")

model.weight.data = torch.zeros(1,
                                dtype=torch.half,
                                device=torch.device(literal_device()))

see_memory_usage("After weight zero")

y.backward(tens_back)
