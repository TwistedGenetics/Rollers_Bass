##############################################################################
# RLmeter - Rollers with a CPU load meter in the MODE display (diagnostic)
# Same DSP and glue as microkorg2/ (unit.cc is shared), built with -DROLLERS_LOADMETER.
#

PROJECT := RLmeter
PROJECT_TYPE := osc

CSRC = header.c \
       $(realpath $(PLATFORM_COMMON_PATH)/dsp/midi_to_hz_lut.c)
CXXSRC = unit.cc

ACSRC =
ACXXSRC =
ASMSRC =
ASMXSRC =

UINCDIR = $(PROJECT_ROOT)/../../core

ULIBDIR =

ULIBS  = -lm
ULIBS += -lc

OPTIM = -O3

UDEFS = -DROLLERS_LOADMETER
