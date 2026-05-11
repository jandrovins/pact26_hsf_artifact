{
	#inputs.nodes_src.url = "path:/nfs/home/Computational/varcila/devshell_tg_apps/nodes";
	#inputs.nosv_src.url = "path:/nfs/home/Computational/varcila/devshell_tg_apps/nosv";
	inputs.jungle.url = "git+ssh://git@github.com/jandrovins/jungle.git?ref=tglib";
	inputs.nodes_src.url = "https://github.com/bsc-pm/nodes/releases/download/version-1.4/nodes-1.4.0.tar.gz";
	inputs.nodes_src.flake = false;
	inputs.nosv_src.url = "https://github.com/bsc-pm/nos-v/releases/download/4.0.0/nos-v-4.0.0.tar.bz2";
	inputs.nosv_src.flake = false;

	outputs = { self, jungle, nodes_src, nosv_src}:
	let
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
    blisOverlay = (final: prev: {

      # Build blis for Fox architecture and without OpenMP
      amd-blis = (prev.amd-blis.override {
        withOpenMP = false;
        withArchitecture = "zen4";
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


	llvmOmpssAffinityBranchOverlay = final: prev: {
		clangOmpss2Unwrapped = (prev.clangOmpss2Unwrapped.override {
			useGit = true;
			gitUrl = "git@bscpm04.bsc.es:varcila/llvm-mono.git";
			gitBranch = "affinity_2";
			gitCommit = "3340e46386495e73cd21c83f6ec44b6dcf0739ad";
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
			CFLAGS = import ./fox_cflags.nix;
			CXXFLAGS = import ./fox_cflags.nix;
		};
	nosvAffinityOverlay = final: prev: {
		nosv = (prev.nosv.overrideAttrs (old: {
			useGit = false;
			src = nosv_src;
			version = "4.0.0";
			#NIX_CFLAGS_COMPILE = (old.NIX_CFLAGS_COMPILE or "") + " -march=native";
			#configureFlags = (old.configureFlags or []) ++ [" CFLAGS=-march=native" " CXXFLAGS=-march=native"];
			env = foxCflags;
		}));
	};

	nodesAffinityOverlay = final: prev: {
		nodes = prev.nodes.overrideAttrs (old:  {
			useGit = false;
			src = nodes_src;
			version = "1.4.0";
			#configureFlags = (old.configureFlags or []) ++ ["CFLAGS=${cflags}" "CXXFLAGS=${cflags}"];
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
				zsh
				nosv
				nodes
				ovni
				gcc 
				clangOmpss2 
				libbacktrace gdb numactl
				ncurses
				cmakeCurses
				ninja
				tampi.debug
				#rustc cargo hwloc sysstat boost btop bigotes
				bear 
				numactl
				wxparaver
				bigotes
				papi
				python3
				autoconf
				automake
				autogen
				libtool
				pkg-config
				#mkl
				btop
				mpi glibc_memusage valgrind
				amd-uprof
				ministat
				valloc
                                amd-blis
                                amd-libflame
				linuxKernel.packages.linux_zen.turbostat


				# more packages here...
			];

			# Include the dependencies needed to build nOS-V, NODES, etc...
			inputsFrom = with pkgs; [ ];
			hardeningDisable= ["all"];

			shellHook = ''
				export CC=clang
				export CXX=clang++
				export NODES_HOME=${pkgs.nodes}
				export NOSV_HOME=${pkgs.nosv}
				export TAMPI_HOME=${pkgs.tampi}
				export BLIS_HOME=${pkgs.amd-blis}
                                export LIBFLAME_HOME=${pkgs.amd-libflame}
				export NIX_ENFORCE_NO_NATIVE=0
				echo "Environment variables set:"
				echo "  CC=clang"
				echo "  CXX=clang++"
				echo "  NODES_HOME=$NODES_HOME"
				echo "  NOSV_HOME=$NOSV_HOME"
				echo "  TAMPI_HOME=$TAMPI_HOME"
				echo "  BLIS_HOME=$BLIS_HOME"
				echo "  LIBFLAME_HOME=$BLIS_HOME"
				'';
		};

	};
}

