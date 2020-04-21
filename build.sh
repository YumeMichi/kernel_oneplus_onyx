#!/bin/bash

# Bash Color
green='\033[01;32m'
red='\033[01;31m'
blink_red='\033[05;31m'
restore='\033[0m'

clear

# Resources
THREAD="-j$(nproc)"
export CROSS_COMPILE=$HOME/Workspace/toolchains/gcc-linaro-4.9.4-2017.01-x86_64_arm-eabi/bin/arm-eabi-

# Kernel Details
DEFCONFIG="onyx_defconfig"

# Paths
KERNEL_DIR=`pwd`
AK_DIR="$HOME/Workspace/AnyKernel3"
ZIP_MOVE="$HOME/Workspace/AK-releases"
ZIMAGE_DIR="$KERNEL_DIR/arch/arm/boot"

# Functions
function clean_all {
    echo
    cd $KERNEL_DIR
    git clean -fdx > /dev/null 2>&1
}

function make_kernel {
    echo
    make $DEFCONFIG
    make savedefconfig
    make $THREAD
}

function make_zip {
    echo
    cd $AK_DIR

    git reset --hard > /dev/null 2>&1
    git clean -fdx > /dev/null 2>&1
    git checkout onyx

    cp -vr $ZIMAGE_DIR/zImage-dtb $AK_DIR/zImage-dtb

    AK_FULL_VER=$AK_VER-$(date +%F | sed s@-@@g)-YumeMichi

    zip -r9 $AK_FULL_VER.zip *
    mv $AK_FULL_VER.zip $ZIP_MOVE

    cd $KERNEL_DIR
}

DATE_START=$(date +"%s")

echo -e "${green}"
echo "-----------------"
echo "Making Kernel:"
echo "-----------------"
echo -e "${restore}"

# Vars
BASE_AK_VER="PolarKernel"
AK_VER="$BASE_AK_VER$VER"
export ARCH=arm
export SUBARCH=arm
export KBUILD_BUILD_USER=violet
export KBUILD_BUILD_HOST=Evergarden

echo

while read -p "Clean stuffs (y/n)? " cchoice
do
case "$cchoice" in
    y|Y )
        clean_all
        echo
        echo "All Cleaned now."
        break
        ;;
    n|N )
        break
        ;;
    * )
        echo
        echo "Invalid try again!"
        echo
        ;;
esac
done

echo

while read -p "Start building (y/n)? " dchoice
do
case "$dchoice" in
    y|Y )
        make_kernel
        make_zip
        break
        ;;
    n|N )
        echo
        echo "Abort!"
        echo
        break
        ;;
    * )
        echo
        echo "Invalid try again!"
        echo
        ;;
esac
done

echo -e "${green}"
echo "-------------------"
echo "Build Completed in:"
echo "-------------------"
echo -e "${restore}"

DATE_END=$(date +"%s")
DIFF=$(($DATE_END - $DATE_START))
echo "Time: $(($DIFF / 60)) minute(s) and $(($DIFF % 60)) seconds."
echo
