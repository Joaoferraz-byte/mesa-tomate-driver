{ lib
, stdenv
, pkg-config
, libusb1
}:

stdenv.mkDerivation {
  pname = "mtm1106-mode";
  version = "0.2.1";

  src = ./.;

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ libusb1 ];

  # The daemon no longer links against libudev (it uses plain libusb
  # enumeration for reconnection tracking), so the build only needs libusb;
  # linking against the system's /lib/libudev.so stub used to produce a
  # "generic linux" binary that NixOS refuses to run (stub-ld error 127).

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
