// Builds os24arm (OS/2 Warp 4.52 for ARM64):
//   - the QEMU-target kernel image (os2warp.img) and a bootable, dd-able
//     ISO wrapping it (os2warp-arm64.iso)
//   - the Raspberry Pi 3+ kernel image (kernel8.img)
//   - the BeagleBone Black (os2warp-bbb.img) and BeagleV-Ahead
//     (os2warp-beaglev-ahead.img) kernel images, each with its uEnv.txt
//   - the QEMU RISC-V kernel image (os2warp-qemu-riscv.img)
//
// Requires an agent with the toolchain from README.md's Prerequisites
// section already installed (clang, lld, llvm-{objcopy,nm,ar,ranlib},
// cmake, xorriso). On a Debian/Ubuntu agent, ci/scripts/install-deps-linux.sh
// installs everything this pipeline needs.
pipeline {
    agent any

    options {
        timestamps()
        disableConcurrentBuilds()
    }

    environment {
        // Debian/Ubuntu agents get their toolchain from install-deps-linux.sh,
        // which installs versioned clang-17/lld-17/llvm-17 packages (no
        // unversioned "clang" symlink). Every other agent (e.g. macOS, per
        // README.md's `brew install llvm` + PATH export) is expected to
        // already have a plain "clang" resolvable on PATH.
        CC = "${fileExists('/etc/debian_version') ? 'clang-17' : 'clang'}"
    }

    stages {
        stage('Install dependencies') {
            when { expression { isUnix() && fileExists('/etc/debian_version') } }
            steps {
                sh 'ci/scripts/install-deps-linux.sh'
            }
        }

        stage('Build QEMU image + ISO') {
            steps {
                sh 'ci/scripts/build-qemu.sh build'
            }
        }

        stage('Build Raspberry Pi 3+ image') {
            steps {
                sh 'ci/scripts/build-rpi.sh build-rpi'
            }
        }

        stage('Build BeagleBone Black image') {
            steps {
                sh 'ci/scripts/build-beagle.sh BBB build-bbb'
            }
        }

        stage('Build BeagleV-Ahead image') {
            steps {
                sh 'ci/scripts/build-beagle.sh BEAGLEV_AHEAD build-beaglev-ahead'
            }
        }

        stage('Build QEMU RISC-V image') {
            steps {
                sh 'ci/scripts/build-beagle.sh QEMU_RISCV build-qemu-riscv'
            }
        }
    }

    post {
        success {
            archiveArtifacts artifacts: 'build/os2warp.img,build/os2warp-arm64.iso,build-rpi/kernel8.img,build-bbb/os2warp-bbb.img,build-bbb/uEnv.txt,build-beaglev-ahead/os2warp-beaglev-ahead.img,build-beaglev-ahead/uEnv.txt,build-qemu-riscv/os2warp-qemu-riscv.img', fingerprint: true
        }
        always {
            cleanWs()
        }
    }
}
