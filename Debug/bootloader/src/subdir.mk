################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../bootloader/src/bootloader.c 

OBJS += \
./bootloader/src/bootloader.o 

C_DEPS += \
./bootloader/src/bootloader.d 


# Each subdirectory must supply rules for building sources it contributes
bootloader/src/%.o bootloader/src/%.su bootloader/src/%.cyclo: ../bootloader/src/%.c bootloader/src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DUSE_HAL_DRIVER -DSTM32F411xE -c -I../Core/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc -I../Drivers/STM32F4xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32F4xx/Include -I../Drivers/CMSIS/Include -I"D:/WORK/Embedded Workspace/Bootloader/bootloader/inc" -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-bootloader-2f-src

clean-bootloader-2f-src:
	-$(RM) ./bootloader/src/bootloader.cyclo ./bootloader/src/bootloader.d ./bootloader/src/bootloader.o ./bootloader/src/bootloader.su

.PHONY: clean-bootloader-2f-src

