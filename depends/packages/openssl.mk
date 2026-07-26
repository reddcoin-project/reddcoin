package=openssl
$(package)_version=3.5.7
$(package)_download_path=https://github.com/openssl/openssl/releases/download/openssl-$($(package)_version)
$(package)_file_name=$(package)-$($(package)_version).tar.gz
$(package)_sha256_hash=a8c0d28a529ca480f9f36cf5792e2cd21984552a3c8e4aa11a24aa31aeac98e8

define $(package)_set_vars
$(package)_config_env=AR="$($(package)_ar)" RANLIB="$($(package)_ranlib)" CC="$($(package)_cc)" RC="$($(package)_windres)"
$(package)_config_opts=--prefix=$(host_prefix) --openssldir=$(host_prefix)/etc/openssl
# The 3.x series defaults to lib64 on 64 bit targets, but everything else in
# depends installs into lib, and that is the only library path the build is
# given. A second directory also breaks Boost detection, which picks the first
# candidate directory that exists and then finds no Boost in it.
$(package)_config_opts+=--libdir=lib
$(package)_config_opts+=no-camellia
$(package)_config_opts+=no-capieng
$(package)_config_opts+=no-cast
$(package)_config_opts+=no-comp
$(package)_config_opts+=no-docs
$(package)_config_opts+=no-dso
$(package)_config_opts+=no-dtls1
$(package)_config_opts+=no-ec_nistp_64_gcc_128
$(package)_config_opts+=no-gost
$(package)_config_opts+=no-idea
$(package)_config_opts+=no-md2
$(package)_config_opts+=no-mdc2
$(package)_config_opts+=no-rc4
$(package)_config_opts+=no-rc5
$(package)_config_opts+=no-rdrand
$(package)_config_opts+=no-rfc3779
$(package)_config_opts+=no-sctp
$(package)_config_opts+=no-seed
$(package)_config_opts+=no-shared
$(package)_config_opts+=no-ssl-trace
$(package)_config_opts+=no-ssl3
$(package)_config_opts+=no-tests
$(package)_config_opts+=no-unit-test
$(package)_config_opts+=no-weak-ssl-ciphers
$(package)_config_opts+=no-whirlpool
$(package)_config_opts+=no-zlib
$(package)_config_opts+=no-zlib-dynamic
$(package)_config_opts+=$($(package)_cflags) $($(package)_cppflags)
$(package)_config_opts_linux=-fPIC -Wa,--noexecstack
$(package)_config_opts_x86_64_linux=linux-x86_64
$(package)_config_opts_i686_linux=linux-generic32
$(package)_config_opts_arm_linux=linux-generic32
$(package)_config_opts_armv7l_linux=linux-generic32
$(package)_config_opts_aarch64_linux=linux-generic64
$(package)_config_opts_mipsel_linux=linux-generic32
$(package)_config_opts_mips_linux=linux-generic32
$(package)_config_opts_powerpc64_linux=linux-ppc64
$(package)_config_opts_powerpc64le_linux=linux-ppc64le
$(package)_config_opts_riscv32_linux=linux-generic32
$(package)_config_opts_riscv64_linux=linux-generic64
$(package)_config_opts_x86_64_darwin=darwin64-x86_64-cc
$(package)_config_opts_x86_64_mingw32=mingw64
$(package)_config_opts_i686_mingw32=mingw
$(package)_config_opts_android=-fPIC
$(package)_config_opts_aarch64_android=linux-generic64
$(package)_config_opts_x86_64_android=linux-generic64
$(package)_config_opts_armv7a_android=linux-generic32
$(package)_config_opts_i686_android=linux-generic32
endef

define $(package)_config_cmds
  ./Configure $($(package)_config_opts)
endef

define $(package)_build_cmds
  $(MAKE) -j1 build_libs libcrypto.pc libssl.pc openssl.pc
endef

define $(package)_stage_cmds
  $(MAKE) DESTDIR=$($(package)_staging_dir) install_sw
endef

define $(package)_postprocess_cmds
  rm -rf share bin etc
endef
