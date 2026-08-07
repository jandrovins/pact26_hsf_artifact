{
	nixConfig = {
		print-build-logs = true;
	};

	#inputs.bscpkgs.url = "path:/nfs/home/Computational/varcila/bscpkgs";
	#inputs.nodes_src.url = "path:/nfs/home/Computational/varcila/devshell_tg_apps/nodes";
	#inputs.nosv_src.url = "path:/nfs/home/Computational/varcila/devshell_tg_apps/nosv";
	inputs.jungle.url = "git+https://github.com/jandrovins/jungle.git?ref=tglib";
	inputs.nodes_src.url = "git+https://github.com/jandrovins/nodes-haffsched.git";
	inputs.nodes_src.flake = false;
	#inputs.nosv_src.url = "git+https://github.com/jandrovins/nos-v-haffsched.git?ref=haffsched-ics&rev=ac69fba1d97ec30942f30f8a02207a243d025b0a";
	inputs.nosv_src.url = "path:/nvme1/varcila/oss_hsf_overhead/nos-v-haffsched";
	inputs.nosv_src.flake = false;
	inputs.ovni_src.url = "path:/nvme1/varcila/ovni";
	inputs.ovni_src.flake = false;

	outputs = { self, jungle, nodes_src, nosv_src, ovni_src}:
	let
	ovniOverlay = final: prev: {
      # Replace ovni by the latest release from git in all packages.
		ovni = (prev.ovni.overrideAttrs (old: {
			useGit = false;
			src = ovni_src;
			version = "ovni-nosv-schedinstr";
			NIX_CFLAGS_COMPILE =
			  (old.NIX_CFLAGS_COMPILE or "")
			  + " -g"
			  + " -fdebug-prefix-map=/build/source=/nvme1/varcila/ovni"
                          + " -fno-omit-frame-pointer"
                          + " -ggdb3";
			dontStrip = true;
			separateDebugInfo = true;
			#NIX_CFLAGS_COMPILE = (old.NIX_CFLAGS_COMPILE or "") + " -march=native";
			#configureFlags = (old.configureFlags or []) ++ [" CFLAGS=-march=native" " CXXFLAGS=-march=native"];
		}));
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

	foxCflags = { 
			CFLAGS = import ./fox_cflags.nix;
			CXXFLAGS = import ./fox_cflags.nix;
		};
	nosvAffinityOverlay = final: prev: {
		nosv = (prev.nosv.overrideAttrs (old: {
			useGit = false;
			src = nosv_src;
			version = "haffsched-ics-noinline";
			buildInputs = (old.buildInputs or []) ++ [ final.elfutils ];
			NIX_CFLAGS_COMPILE =
			  (old.NIX_CFLAGS_COMPILE or "")
			  + " -g"
			  + " -fdebug-prefix-map=/build/source=/nvme1/varcila/oss_hsf_overhead/nos-v-haffsched"
                          + " -fno-omit-frame-pointer"
                          + " -ggdb3"
                          + " -rdynamic"
				;
			dontStrip = true;
			separateDebugInfo = true;
			#NIX_CFLAGS_COMPILE = (old.NIX_CFLAGS_COMPILE or "") + " -march=native";
			#configureFlags = (old.configureFlags or []) ++ [" CFLAGS=-march=native" " CXXFLAGS=-march=native"];
		}));
	};

	nodesAffinityOverlay = final: prev: {
		nodes = prev.nodes.overrideAttrs (old:  {
			useGit = false;
			src = nodes_src;
			version = "haffsched";
			NIX_CFLAGS_COMPILE =
			  (old.NIX_CFLAGS_COMPILE or "")
			  + " -g"
			  + " -fdebug-prefix-map=/build/source=/nvme1/varcila/oss_hsf_overhead/nodes-haffsched"
                          + " -fno-omit-frame-pointer"
                          + " -ggdb3";
			dontStrip = true;
			separateDebugInfo = true;
			#configureFlags = (old.configureFlags or []) ++ ["CFLAGS=${cflags}" "CXXFLAGS=${cflags}"];
		});
	};

	pkgs = import jungle.inputs.nixpkgs {
			system = "x86_64-linux";
			overlays = [
				jungle.bscOverlay
				llvmOmpssAffinityBranchOverlay
				nosvAffinityOverlay
				nodesAffinityOverlay
				ovniOverlay
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
papi
amd-uprof
				gcc 
				clangOmpss2 
				libbacktrace 
				gdb 
				#rustc cargo hwloc sysstat boost btop bigotes
				bear 
				numactl
				autoconf
tmux
perl
				automake
				autogen
				libtool
				pkg-config
screen
systemtap-sdt
elfutils
				hwloc
wxparaver
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
				export PAPI_HOME=${pkgs.papi}
				export OVNI_HOME=${pkgs.ovni}
				export NIX_ENFORCE_NO_NATIVE=0
				echo "Environment variables set:"
				echo "  CC=clang"
				echo "  CXX=clang++"
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

