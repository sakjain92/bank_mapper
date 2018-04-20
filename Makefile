# Location of the CUDA Toolkit
CUDA_PATH ?= "/usr/local/cuda-9.1"
HOST_COMPILER	?= g++
NVCC			:= $(CUDA_PATH)/bin/nvcc -ccbin $(HOST_COMPILER)

HOST_ARCH   	:= $(shell uname -m)
TARGET_ARCH 	?= $(HOST_ARCH)
ifneq (,$(filter $(TARGET_ARCH),x86_64))
TARGET_SIZE := 64
else
$(error ERROR - unsupported value $(TARGET_ARCH) for TARGET_ARCH!)
endif

NVCCFLAGS   	:= -m${TARGET_SIZE} -g -O0
# Gencode arguments
SMS ?= 30 35 37 50 52 60 61 70
ifeq ($(GENCODE_FLAGS),)
# Generate SASS code for each SM architecture listed in $(SMS)
$(foreach sm,$(SMS),$(eval GENCODE_FLAGS += -gencode arch=compute_$(sm),code=sm_$(sm)))

# Generate PTX code from the highest SM architecture in $(SMS) to guarantee forward-compatibility
HIGHEST_SM := $(lastword $(sort $(SMS)))
ifneq ($(HIGHEST_SM),)
GENCODE_FLAGS += -gencode arch=compute_$(HIGHEST_SM),code=compute_$(HIGHEST_SM)
endif
endif


LCC				:=gcc
LCFLAGS			:=-Werror -Wall -O1 -g3


KOBJECT=kam
OBJECT=bank_test_cpu bank_test_gpu

all: $(OBJECT) $(KOBJECT)

bank_test_cpu: bank_test.c common.h
	$(LCC) $(LCFLAGS) -DEVAL_CPU=1 -o $@ $?

bank_test_gpu: bank_test.c gpu_cuda.cu common.h
	$(NVCC) $(NVCCFLAGS) $(GENCODE_FLAGS) -DEVAL_GPU=1 -o gpu_cuda.o -c gpu_cuda.cu
	$(LCC) $(LCFLAGS) -DEVAL_GPU=1 -o bank_test_gpu.o -c bank_test.c 
	$(NVCC) $(NVCCFLAGS) $(GENCODE_FLAGS) -o bank_test_gpu bank_test_gpu.o gpu_cuda.o

obj-m += $(KOBJECT).o

$(KOBJECT):
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm -f $(OBJECT) *.o
