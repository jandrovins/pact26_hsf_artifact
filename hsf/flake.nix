{
	inputs.jungle.url = "git+ssh://git@github.com/jandrovins/jungle.git?ref=tglib";
	inputs.nodes_src.url = "git+ssh://git@github.com/jandrovins/nodes-haffsched.git";
	inputs.nodes_src.flake = false;
	inputs.nosv_src.url = "git+ssh://git@github.com/jandrovins/nos-v-haffsched.git?ref=hwc-perf-analysis&rev=f6c19c404b0d1b042a79492479259af195b375ef";
	inputs.nosv_src.flake = false;

	outputs = { self, jungle, nodes_src, nosv_src}:
	let
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

		foxCflags = let
						# try environment variable first
						hostEnvTry = builtins.tryEval (builtins.getEnv "HOSTNAME");
						hostEnv = if hostEnvTry.success then hostEnvTry.value else "";

						# fallback to /etc/hostname when env is empty
						hostFileTry = builtins.tryEval (builtins.readFile "/etc/hostname");
						hostFileRaw = if hostFileTry.success then hostFileTry.value else "";
						# strip newlines/carriage returns
						hostFile = builtins.replaceStrings ["\n" "\r"] ["" ""] hostFileRaw;

						# choose host and record source
						host = if hostEnv != "" then hostEnv else hostFile;
						source = if hostEnv != "" then "env" else if hostFile != "" then "/etc/hostname" else "none";

						# determine whether to use fox cflags (empty host counts as use)
						useFoxVal = host == "" || (builtins.match ".*[Ff][Oo][Xx].*" host != null);

						useFox = builtins.trace (
							if useFoxVal
							then "Using fox_cflags (source='" + source + "', HOSTNAME='" + host + "')"
							else "Not using fox_cflags (source='" + source + "', HOSTNAME='" + host + "')"
						) useFoxVal;
				in
				if useFox then {
						CFLAGS = import ./fox_cflags.nix;
						CXXFLAGS = import ./fox_cflags.nix;
				} else { };
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
				( tglib.override {
					gitCommit = "d78d0a4463385344e07ef265881a670ceda04823";
				} )
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

