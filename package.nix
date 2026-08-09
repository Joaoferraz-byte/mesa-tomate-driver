{ lib
, stdenv
, pkg-config
, libusb1
}:

stdenv.mkDerivation {
  pname = "mtm1106-mode";
  version = "0.2.0";

  src = ./.;

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ libusb1 ];

  # The daemon only needs libudev at link time; on NixOS it is provided by
  # the udev library that ships with systemd's core (already in the closure),
  # so we link against the udev headers/libs from it directly without adding
  # a package dependency that does not exist in nixpkgs.
  env.LIBUDEV_LIBS = "-ludev";

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    ./mtm1106-mode --self-test
    runHook postCheck
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 mtm1106-mode $out/bin/mtm1106-mode
    install -Dm755 mtm1106-daemon $out/bin/mtm1106-daemon
    runHook postInstall
  '';

  meta = {
    description = "USB mode activator for the MTM-1106 / T501 tablet";
    homepage = "https://github.com/Joaoferraz-byte/mesa-tomate-driver";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "mtm1106-daemon";
  };
}
