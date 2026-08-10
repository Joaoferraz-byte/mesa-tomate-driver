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
      description = "Package providing the mtm1106-mode executables.";
    };

    mode = lib.mkOption {
      type = lib.types.enum [ "daemon" "oneshot" ];
      default = "daemon";
      description = ''
        How the tablet is driven. `daemon` runs a userspace driver that
        switches the tablet to full-area mode and injects pen and hotkey
        events through uinput, bypassing the kernel hid-generic driver
        (which caches the 8-byte mobile-area descriptor and restricts the
        active area). `oneshot` only sends the mode-switch reports once
        and lets the kernel handle the device; this keeps the restricted
        mobile area on most kernel versions, so prefer `daemon`.
      '';
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
        Automatically start the selected mode when the tablet is connected.
        For `daemon` mode the service keeps running while the tablet is
        attached and re-handles reconnects; for `oneshot` mode it runs once
        per USB add event.
      '';
    };

    enableKernelReprobe = lib.mkOption {
      type = lib.types.bool;
      default = true;
      description = ''
        (oneshot mode only) After sending the mode-switch reports, force a
        USB re-probe of the tablet through the kernel sysfs entries. Kept
        for compatibility; in daemon mode the tablet is driven entirely
        from userspace and this option has no effect.
      '';
    };

    environment = lib.mkOption {
      type = lib.types.attrsOf lib.types.str;
      default = {};
      description = ''Environment variables to pass to the daemon or mode-switch service.'';
      example = lib.literalExpression ''{
        MTM1106_CONTACT_THRESHOLD = "300";
        MTM1106_DEBUG_RAW = "0";
      }'';
    };
  };

  config = lib.mkMerge [
    (lib.mkIf cfg.enable {
      environment.systemPackages = [ cfg.package ];
    })

    (lib.mkIf (cfg.enable && cfg.autoStart && cfg.mode == "daemon") {
      services.udev.extraRules = ''
        ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="08f2", ATTR{idProduct}=="6811", TAG+="systemd", ENV{SYSTEMD_WANTS}+="mtm1106-mode.service"
      '';

      systemd.services."mtm1106-mode" = {
        description = "Userspace driver for the MTM-1106/T501 tablet (full-area mode via uinput)";
        documentation = [ "https://github.com/Joaoferraz-byte/mesa-tomate-driver" ];
        after = [ "systemd-udev-settle.service" ];
        wantedBy = [ "multi-user.target" ];
        environment = cfg.environment;
        serviceConfig = {
          Type = "simple";
          ExecStart = "${cfg.package}/bin/mtm1106-daemon";
          Restart = "always";
          RestartSec = "2";
          User = "root";
          NoNewPrivileges = true;
          PrivateTmp = true;
          ProtectHome = true;
          ProtectSystem = "strict";
          ReadWritePaths = [ "/run" "/dev/uinput" ];
        };
      };
    })

    (lib.mkIf (cfg.enable && cfg.autoStart && cfg.mode == "oneshot") {
      services.udev.extraRules = ''
        ACTION=="add", SUBSYSTEM=="usb", ATTR{idVendor}=="08f2", ATTR{idProduct}=="6811", TAG+="systemd", ENV{SYSTEMD_WANTS}+="mtm1106-mode.service"
      '';

      systemd.services."mtm1106-mode" = {
        description = "Activate full-area mode on the MTM-1106/T501 tablet";
        documentation = [ "https://github.com/Joaoferraz-byte/mesa-tomate-driver" ];
        after = [ "systemd-udev-settle.service" ];
        environment = cfg.environment;
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
