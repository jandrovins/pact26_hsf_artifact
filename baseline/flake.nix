{
	inputs.jungle.url = "git+https://github.com/jandrovins/jungle.git?ref=tglib";
	outputs = { self, jungle }:
	let
	# Target architecture. Defaults to a portable build; set HSF_ARCH=znver4 on
	# the paper's AMD EPYC 9684X (Genoa-X) machine for the faithful external
	# baseline. (Read impurely; invocations use `nix develop --impure`.)
	hsfArch = let e = builtins.getEnv "HSF_ARCH"; in if e == "" then "native" else e;
	isZen4 = hsfArch == "znver4";
	archCflags = if isZen4 then import ./fox_cflags.nix
		else "-march=${hsfArch} -mtune=${hsfArch}";
    blisOverlay = (final: prev: {

      # Build blis with OpenMP; zen4 kernels only for the faithful
      # HSF_ARCH=znver4 build, otherwise a portable generic BLIS.
      amd-blis = (prev.amd-blis.override {
        withOpenMP = true;
        withArchitecture = if isZen4 then "zen4" else "generic";
      }).overrideAttrs (old: {
	#configureFlags =  [ "--enable-debug" ] ++ (old.configureFlags or [ ]);
	dontStrip = true;
        hardeningDisable = [ "all" ];
      });

      amd-libflame = (prev.amd-libflame.override {
        withOpenMP = true;
        amd-blis = final.amd-blis;
      }).overrideAttrs (old: {
				cmakeFlags = (old.cmakeFlags or [ ]) ++ [
					"-DCMAKE_BUILD_TYPE=RelWithDebInfo"
					"-DENABLE_DTL=ON"
					"-DENABLE_AMD_FLAGS=ON"
					"-DENABLE_AMD_OPT=ON"
					"-DAOCL_ROOT=${final.amd-blis}"
					"-DENABLE_AOCL_BLAS=ON"
					"-DAOCL_BLAS_LIB=${final.amd-blis}/lib/libblis-mt.so"
					"-DAOCL_BLAS_INCLUDE_DIR=${final.amd-blis}/include/blis"
					"-DENABLE_SET_LIB_VERSION=\"5.1.0Build\""
				] ++ (if isZen4 then [ "-DLF_ISA_CONFIG=avx512" ] else [ ]);
				env = (old.env or {}) // { NIX_CFLAGS_COMPILE = ((old.env or {}).NIX_CFLAGS_COMPILE or (old.NIX_CFLAGS_COMPILE or "")) + " -DBLIS_DISABLE_CBLAS"; };
				NIX_LDFLAGS = (old.NIX_LDFLAGS or "") + " -L${final.amd-blis}/lib -lblis-mt -rpath ${final.amd-blis}/lib";
				dontStrip = true;
        hardeningDisable = [ "all" ];
      });
        });


	foxCflags = {
			CFLAGS = archCflags;
			CXXFLAGS = archCflags;
		};

	pkgs = import jungle.inputs.nixpkgs {
			system = "x86_64-linux";
			overlays = [
				jungle.bscOverlay
				blisOverlay
			];
			config.allowUnfree = true;
		};
	in {
		packages.x86_64-linux = {
			inherit (pkgs);
		};
		devShells.x86_64-linux.default = pkgs.mkShell {
			pname = "devshell";
			buildInputs = with pkgs; [
				zsh
				gcc 
				clang
				llvmPackages.openmp
				llvmPackages.openmp.dev
				gdb numactl
				ncurses
				cmakeCurses
				ninja
				bear 
				papi
				python3
				autoconf
				automake
				autogen
				libtool
				pkg-config
				btop
				mpi glibc_memusage valgrind
				amd-uprof
				ministat
				valloc
				aocl-utils
				amd-blis
				amd-libflame
				mkl
				tbb
				linuxKernel.packages.linux_zen.turbostat
				# more packages here...
			];

			# Include the dependencies needed to build nOS-V, NODES, etc...
			inputsFrom = with pkgs; [ ];
			hardeningDisable= ["all"];

			shellHook = ''
				export CC=${pkgs.clang}/bin/clang
				export CXX=${pkgs.clang}/bin/clang++
				export BLIS_HOME=${pkgs.amd-blis}
				export LIBFLAME_HOME=${pkgs.amd-libflame}
				export NIX_ENFORCE_NO_NATIVE=0
				echo "Environment variables set:"
				echo "  CC=$CC"
				echo "  CXX=$CXX"
				echo "  BLIS_HOME=$BLIS_HOME"
				echo "  LIBFLAME_HOME=$LIBFLAME_HOME"
				'';
		};

	};
}

