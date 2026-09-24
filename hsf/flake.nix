{
	inputs.jungle.url = "git+https://github.com/jandrovins/jungle.git?ref=tglib&rev=dfc4fd0dc5054d2483c027b715bee3a7cff90b20";
	inputs.nodes_src.url = "git+https://github.com/jandrovins/nodes-haffsched.git";
	inputs.nodes_src.flake = false;
	inputs.nosv_src.url = "git+https://github.com/jandrovins/nos-v-haffsched.git?ref=hwc-perf-analysis&rev=f6c19c404b0d1b042a79492479259af195b375ef";
	inputs.nosv_src.flake = false;

	outputs = { self, jungle, nodes_src, nosv_src}:
	let
	# Target architecture for compiled code. Defaults to a portable "native"
	# build so the artifact is Functional on any host; set HSF_ARCH=znver4 on
	# the paper's AMD EPYC 9684X (Genoa-X) machine for faithful reproduction.
	# (Read impurely; all invocations use `nix develop --impure`.)
	hsfArch = let e = builtins.getEnv "HSF_ARCH"; in if e == "" then "native" else e;
	isZen4 = hsfArch == "znver4";
	archCflags = if isZen4 then import ./fox_cflags.nix
		else "-march=${hsfArch} -mtune=${hsfArch}";

	# tglib from the author's public GitHub over HTTPS (jungle's default gitUrl
	# is SSH). Applied as an overlay so both buildInputs and TGLIB_HOME use it.
	tglibOverlay = final: prev: {
		tglib = prev.tglib.override {
			gitUrl = "https://github.com/jandrovins/tglib.git";
			gitBranch = "main";
			gitCommit = "d78d0a4463385344e07ef265881a670ceda04823";
		};
	};

    blisOverlay = (final: prev: {

      # Build blis without OpenMP; use zen4 kernels only for the faithful
      # HSF_ARCH=znver4 build, otherwise a portable generic BLIS.
      amd-blis = (prev.amd-blis.override {
        withOpenMP = false;
        withArchitecture = if isZen4 then "zen4" else "generic";
      }).overrideAttrs (old: {
        hardeningDisable = [ "all" ];
      });

      # Disable OpenMP in flame
      amd-libflame = (prev.amd-libflame.override {
        withOpenMP = false;
      }).overrideAttrs (old: {
        hardeningDisable = [ "all" ];
      });	
	});

	ovniOverlay = final: prev: {
      # Replace ovni by the latest release from git in all packages.
      ovni = (prev.ovni.override {
        useGit = true;
        gitUrl = "https://github.com/bsc-pm/ovni.git";
        gitCommit = "275aea9a469759378962437f2af449f0678dc295";
      }).overrideAttrs {
        # Fix version
        version = "1.13.0";
		__intentionallyOverridingVersion = true;
      };
    };


	llvmOmpssAffinityBranchOverlay = final: prev: {
		clangOmpss2Unwrapped = (prev.clangOmpss2Unwrapped.override {
			useGit = true;
			gitUrl = "https://github.com/jandrovins/llvm-mono-ompss2.git";
			gitBranch = "affinity_2";
			# Squashed snapshot of the bscpm04 affinity_2 tip 3340e46 (same source
			# tree, no history, so it fits GitHub's push limit). Tree-identical, so
			# the built clangOmpss2 is byte-for-byte the same.
			gitCommit = "a094198c7ca7fc2414c486010f49b8f77eae5e8b";
		});
	};

	mpichDebugOverlay = final: prev: {
		mpich = (
			prev.mpich.override {
				enableDebug = true;
			}
		);
	};

		foxCflags = {
					CFLAGS = archCflags;
					CXXFLAGS = archCflags;
			};
	nosvAffinityOverlay = final: prev: {
		nosv = (prev.nosv.overrideAttrs (old: {
			useGit = false;
			src = nosv_src;
			version = "haffsched-local-hwc-counters";
			NIX_CFLAGS_COMPILE =
				(old.NIX_CFLAGS_COMPILE or "")
				+ " -g";
			dontStrip = true;
			separateDebugInfo = true;
			#configureFlags = (old.configureFlags or []) ++ [" CFLAGS=-march=native" " CXXFLAGS=-march=native"];
			env = foxCflags;
		}));
	};

	nodesAffinityOverlay = final: prev: {
		nodes = prev.nodes.overrideAttrs (old:  {
			useGit = false;
			src = nodes_src;
			version = "haffsched-debug2";
			#configureFlags = (old.configureFlags or []) ++ ["CFLAGS=${cflags}" "CXXFLAGS=${cflags}"];
			NIX_CFLAGS_COMPILE =
				(old.NIX_CFLAGS_COMPILE or "")
				+ " -g";
			dontStrip = true;
			separateDebugInfo = true;
			env = foxCflags;
		});
	};

	pkgs = import jungle.inputs.nixpkgs {
			system = "x86_64-linux";
			overlays = [
				jungle.bscOverlay
				llvmOmpssAffinityBranchOverlay
				nosvAffinityOverlay
				nodesAffinityOverlay
				#mpichDebugOverlay
				ovniOverlay
				blisOverlay
				tglibOverlay
			];
			config.allowUnfree = true;
		};
	in {
		packages.x86_64-linux = {
			inherit (pkgs) nosv nodes;
		};
		devShells.x86_64-linux.default = pkgs.mkShell {
			pname = "devshell";
			buildInputs = with pkgs; [
				nosv
				nodes
				ovni
				gcc 
				clangOmpss2 
				libbacktrace gdb numactl
				ncurses
				ninja
				tampi.debug
				bear 
				numactl
				bigotes
				papi
				( python3.withPackages (p: with p; [
					pandas
					numpy
					matplotlib
					seaborn
					pip
					psutil
					multiprocess
				]) )
				autoconf
				automake
				autogen
				libtool
				pkg-config
				hwloc
				linuxKernel.packages.linux_zen.turbostat
				tglib
				mpi
				amd-blis
				amd-libflame
			];

			# Include the dependencies needed to build nOS-V, NODES, etc...
			inputsFrom = with pkgs; [ ];
			hardeningDisable= ["all"];

			shellHook = ''
				export CC=clang
				export CXX=clang++
				export PAPI_HOME=${pkgs.papi}
				export OVNI_HOME=${pkgs.ovni}
				export NODES_HOME=${pkgs.nodes}
				export NOSV_HOME=${pkgs.nosv}
				export TAMPI_HOME=${pkgs.tampi}
				export BLIS_HOME=${pkgs.amd-blis}
				export LIBFLAME_HOME=${pkgs.amd-libflame}
				export TGLIB_HOME=${pkgs.tglib}
				export NIX_ENFORCE_NO_NATIVE=0
				echo "Environment variables set:"
				echo "  CC=clang"
				echo "  CXX=clang++"
				echo "  PAPI_HOME=$PAPI_HOME"
				echo "  OVNI_HOME=$OVNI_HOME"
				echo "  NODES_HOME=$NODES_HOME"
				echo "  NOSV_HOME=$NOSV_HOME"
				echo "  TAMPI_HOME=$TAMPI_HOME"
				echo "  BLIS_HOME=$BLIS_HOME"
				echo "  LIBFLAME_HOME=$LIBFLAME_HOME"
				echo "  TGLIB_HOME=$TGLIB_HOME"
				'';
		};

	};
}

