# spi-bridge
May work if compiled on your target

but I cross-compiled with the Yocto SDK (https://github.com/gumstix/yocto-manifest/wiki/Cross-Compile-with-Yocto-SDK)

I had to edit workspace/sdk/environment-setup-cortexa8hf-vfp-neon-poky-linux-gnueabi and add '-pthread'

change the line 'export CC=" . . .' to:

```
export CC="arm-poky-linux-gnueabi-gcc  -march=armv7-a -mthumb-interwork -mfloat-abi=hard -mfpu=neon -mtune=cortex-a8 --sysroot=$SDKTARGETSYSROOT -pthread"
```
