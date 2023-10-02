# Based on https://gitlab.com/ubports/porting/community-ports/android9/oneplus-5

########################################################################
# Kernel
########################################################################

VARIANT = android
KERNEL_BASE_VERSION = 4.4.153
KERNEL_BOOTIMAGE_CMDLINE = androidboot.hardware=qcom user_debug=31 msm_rtb.filter=0x37 ehci-hcd.park=3 lpm_levels.sleep_disabled=1 sched_enable_hmp=1 sched_enable_power_aware=1 service_locator.enable=1 swiotlb=2048 androidboot.usbconfigfs=true androidboot.usbcontroller=a800000.dwc3 firmware_class.path=/vendor/firmware_mnt/image loop.max_part=7 console=tty0 systemd.unified_cgroup_hierarchy=0 droidian.lvm.prefer
DEVICE_VENDOR = oneplus
DEVICE_MODEL = msm8998
DEVICE_FULL_NAME = OnePlus 5/5T
KERNEL_CONFIG_USE_FRAGMENTS = 0
KERNEL_CONFIG_USE_DIFFCONFIG = 0
KERNEL_DEFCONFIG = lineage_oneplus5_defconfig
KERNEL_IMAGE_WITH_DTB = 0

# Path to the DTB
# If you leave this undefined, an attempt to find it automatically
# will be made.
#KERNEL_IMAGE_DTB = arch/arm64/boot/dts/qcom/my_dtb.dtb
# TODO: can this still not be used to specify more than one?

KERNEL_IMAGE_WITH_DTB_OVERLAY = 0
KERNEL_IMAGE_WITH_DTB_OVERLAY_IN_KERNEL = 0

KERNEL_BOOTIMAGE_PAGE_SIZE = 4096
KERNEL_BOOTIMAGE_BASE_OFFSET = 0x00000000
KERNEL_BOOTIMAGE_KERNEL_OFFSET = 0x00008000
KERNEL_BOOTIMAGE_INITRAMFS_OFFSET = 0x01000000
KERNEL_BOOTIMAGE_SECONDIMAGE_OFFSET = 0x00f00000
KERNEL_BOOTIMAGE_TAGS_OFFSET = 0x00000100

# Specify boot image security patch level if needed
#KERNEL_BOOTIMAGE_PATCH_LEVEL = 2022-04-05

# Required for header version 2, ignore otherwise
#KERNEL_BOOTIMAGE_DTB_OFFSET = 0x1f00000

KERNEL_BOOTIMAGE_VERSION = 0

########################################################################
# Android verified boot (AVB)
########################################################################

DEVICE_VBMETA_REQUIRED = 0

########################################################################
# Automatic flashing on package upgrades
########################################################################

FLASH_ENABLED = 1
FLASH_IS_AONLY = 1
FLASH_IS_LEGACY_DEVICE = 1
FLASH_IS_EXYNOS = 0

# Device manufacturer. This must match the `ro.product.vendor.manufacturer`
# Android property. If you don't want to specify this, leave it undefined,
# FLASH_INFO_CPU will be checked instead.
#FLASH_INFO_MANUFACTURER = OnePlus

# Device model. This must match the `ro.product.vendor.model`
# Android property. If you don't want to specify this, leave it undefined,
# FLASH_INFO_CPU will be checked instead.
# FIXME: what about multiple models?! -> FLASH_INFO_MODELS = 5 5T (similar to FLASH_INFO_DEVICE_IDS below)
#FLASH_INFO_MODEL = 5T
#FLASH_INFO_MODEL = 5

FLASH_INFO_CPU = Qualcomm Technologies, Inc MSM8998
FLASH_INFO_DEVICE_IDS = msm8998

########################################################################
# Kernel build settings
########################################################################

BUILD_CROSS = 1
BUILD_TRIPLET = aarch64-linux-android-
BUILD_CLANG_TRIPLET = aarch64-linux-gnu-
BUILD_CC = aarch64-linux-android-gcc-4.9
DEB_TOOLCHAIN = linux-initramfs-halium-generic:arm64, binutils-aarch64-linux-gnu, gcc-4.9-aarch64-linux-android, g++-4.9-aarch64-linux-android, libgcc-4.9-dev-aarch64-linux-android-cross
DEB_BUILD_ON = amd64
DEB_BUILD_FOR = arm64
KERNEL_ARCH = arm64
KERNEL_BUILD_TARGET = Image.gz-dtb
# TODO: is Image-dtb.gz different? it's already documented
