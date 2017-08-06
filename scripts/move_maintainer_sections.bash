# Select various bits out of MAINTAINERS and create new file and commit
function separate_and_commit () {
    local filename=$1
    shift
    local comment=$1
    shift
    local selection=$@

    cd MAINTAINERS
    perl ../scripts/parse-maintainers.pl $selection
    mv MAINTAINERS.new MAINTAINERS
    mv SECTION.new $filename
    cd ..
    cat << EOF > commit.msg
MAINTAINERS: Move $comment sections to separate file

Make it easier to handle merge conflicts.
EOF
    git add MAINTAINERS/$filename
    git commit -s -F commit.msg MAINTAINERS/MAINTAINERS MAINTAINERS/$filename
    rm commit.msg
}

# First move the MAINTAINERS file into a separate MAINTAINERS directory
git mv MAINTAINERS MAINTAINERS.old
mkdir MAINTAINERS
git mv MAINTAINERS.old MAINTAINERS/MAINTAINERS
cat << EOF > commit.msg
MAINTAINERS: Move to MAINTAINERS directory

This allows breaking up the very large and difficult to merge
MAINTAINERS files into separate files
EOF
git commit -s -F commit.msg MAINTAINERS/MAINTAINERS
rm commit.msg

# Sort the MAINTAINERS file appropriately

cd MAINTAINERS
perl ../scripts/parse-maintainers.pl
mv MAINTAINERS.new MAINTAINERS
cd ..
cat << EOF > commit.msg
MAINTAINERS: Reorder and alphabetize file sections

Move the sections patterns into a standardized order.
EOF
git commit -s -F commit.msg MAINTAINERS/MAINTAINERS 
rm commit.msg

# Move the kernel related sections

separate_and_commit kernel kernel "\\nF:\\s*kernel/(?:.*/)*\\n" "\\nF:\\s*kernel/(?:.*/)*(?:.*\\.[ch]|.*\\*)?\\n"
separate_and_commit filesystems "filesystems" "\\nF:\\s*fs/"
separate_and_commit hypervisors "hypervisor subsystems and drivers" "\\nL:\\s*xen-devel" "\\nL:\\s*virtualization" "Hyper-V"

# Move arch specific files

for arch in $(find arch -maxdepth 1 -type d | \
		  sed 's@^arch/@@' | \
		  awk '{ if (NR > 1) { print; } }' | \
		  sort) ; do
    separate_and_commit arch_$arch arch/$arch "\\nF:\\s*arch/$arch/(?:.*/)*\\n"
done

# Move various subsystems and driver sections

separate_and_commit networking_core "networking core" "\\nF:\\s*net/"
separate_and_commit drivers_wireless "wireless drivers" "\\nF:\\s*drivers/net/wireless/" "\\bWIRELESS\\b"
separate_and_commit drivers_ethernet "ethernet drivers" "\\nF:\\s*drivers/net/ethernet/"
separate_and_commit drivers_scsi "scsi drivers" "\\nF:\\s*drivers/scsi/"
separate_and_commit drivers_usb "usb drivers" "\\nF:\\s*drivers/usb/"
separate_and_commit drivers_media "media drivers" "\\nF:\\s*drivers/media/"
separate_and_commit drivers_watchdog "watchdog drivers" "\\nF:\\s*drivers/watchdog/"
separate_and_commit sound "sound subsystems and drivers" "\\nF:\\s*sound/"
separate_and_commit hardware_monitoring "monitoring subsystem and drivers" "\\nL:\\s*linux-hwmon" "\\nF:\\s*drivers/hwmon/"
separate_and_commit acpi "ACPI subsystem and drivers" "\\nL:\\s*linux-acpi" "\\nF:\\s*drivers/acpi/"
separate_and_commit drivers_input "input drivers" "\\nL:\\s*linux-input" "\\nF:\\s*drivers/input/"
separate_and_commit drivers_video "video drivers" "\\nL:\\s*linux-input" "\\nF:\\s*drivers/video/"
separate_and_commit drivers_gpio "gpio drivers" "\\nL:\\s*linux-gpio" "\\nF:\\s*drivers/gpio/"
separate_and_commit drivers_serial "serial/tty drivers" "\\nL:\\s*linux-serial" "\\nF:\\s*drivers/tty/"
separate_and_commit drivers_gpu_drm "GPU/DRM drivers" "\\nF:\\s*drivers/gpu/"
separate_and_commit drivers_i2c "I2C drivers" "\\nL:\\s*linux-i2c" "\\nF:\\s*drivers/i2c/"
separate_and_commit drivers_staging "staging drivers" "\\nL:\\s*devel\@driverdev" "\\nF:\\s*drivers/staging/"
separate_and_commit power "power management and drivers" "\\nL:\\s*linux-pm" "\\nF:\\s*drivers/cpufreq/" "\\nF:\\s*drivers/cpuidle/" "\\nF:\\s*drivers/devfreq/"
separate_and_commit pci "PCI drivers" "\\nL:\\s*linux-pci" "\\nF:\\s*drivers/pci/"
separate_and_commit platform-driver-x86 "x86 platform drivers" "\\nL:\\s*platform-driver-x86" "\\nF:\\s*drivers/platform/x86/"
