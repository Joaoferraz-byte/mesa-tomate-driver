{ config, lib, pkgs, ... }:

with lib;

let
  cfg = config.services."mtm1106-mode";
  mtm1106-mode = pkgs.callPackage ./package.nix { };
  profileArg = [ "--profile" cfg.profile ];
  reprobeArg = lib.optional cfg.enableKernelReprobe "--reprobe";
in
{
  options.services."mtm1106-mode" = {
    enable = lib.mkEnableOption "the MTM-1106/T501 full-area USB mode activator";

    package = lib.mkOption {
      type = lib.types.package;
      default = mtm1106-mode;
      defaultText = lib.literalExpression "pkgs.callPackage ./package.nix { }";
      description = "Package providing the mtm1106-mode executable.";
    };

    profile = lib.mkOption {
      type = lib.types.enum [ "digimend" "mx002" ];
      default = "digimend";
      description = ''
        USB report profile. `digimend` is the complete four-report sequence;
        `mx002` is the single-report sequence validated by the mx002 reference
        driver.
      '';
    };

    autoStart = lib.mkOption {
      type = lib.types.bool;
      default = false;
      description = ''
        Automatically apply the selected profile via udev when the tablet is connected.
      '';
    };

    enableKernelReprobe = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = ''
        After sending the mode-switch reports, force a USB re-probe of the
        tablet through the kernel sysfs entries. This makes hid-generic/
        hid-t501 reload the 64-byte full-area report descriptor instead of
        keeping the cached 8-byte "mobile area" one; keep enabled unless
        the tablet fails to reconnect.
      '';
    };
  };

  config = lib.mkMerge [
    (lib.mkIf cfg.enable {
      environment.systemPackages = [ cfg.package ];
    })

    (lib.mkIf (cfg.enable && cfg.autoStart) {
      services.udev.extraRules = ''
        ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="08f2", ATTR{idProduct}=="6811", TAG+="systemd", ENV{SYSTEMD_WANTS}+="mtm1106-mode.service"
      '';

      systemd.services."mtm1106-mode" = {
        description = "Activate full-area mode on the MTM-1106/T501 tablet";
        documentation = [ "https://github.com/Joaoferraz-byte/mesa-tomate-driver" ];
        after = [ "systemd-udev-settle.service" ];
        serviceConfig = {
          Type = "oneshot";
          ExecStart = lib.escapeShellArgs ([ "${cfg.package}/bin/mtm1106-mode" ] ++ profileArg ++ reprobeArg);
          User = "root";
          NoNewPrivileges = true;
          PrivateTmp = true;
          ProtectHome = true;
          ProtectSystem = "strict";
          ReadWritePaths = [ "/run" ];
        };
      };
    })
  ];
}
