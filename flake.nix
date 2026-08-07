{
  description = "MTM-1106 / T501 USB full-area mode activator";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forEachSystem = f: nixpkgs.lib.genAttrs systems (system: f {
        inherit system;
        pkgs = import nixpkgs {
          inherit system;
        };
      });
    in {
      packages = forEachSystem ({ pkgs, ... }: {
        default = pkgs.callPackage ./package.nix { };
        mtm1106-mode = pkgs.callPackage ./package.nix { };
      });

      checks = forEachSystem ({ pkgs, ... }: {
        mtm1106-mode = pkgs.callPackage ./package.nix { };
      });

      nixosModules.default = import ./nixos-module.nix;
      nixosModules.mtm1106-mode = import ./nixos-module.nix;
    };
}
