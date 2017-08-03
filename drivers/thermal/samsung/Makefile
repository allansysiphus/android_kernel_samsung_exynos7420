#
# Samsung thermal specific Makefile
#
obj-$(CONFIG_EXYNOS_THERMAL)			+= exynos_thermal.o
exynos_thermal-y				:= exynos_tmu.o

ifeq ($(CONFIG_SOC_EXYNOS7420),y)
obj-$(CONFIG_EXYNOS_THERMAL)	+= exynos7420_thermal.o cal_tmu7420.o
endif