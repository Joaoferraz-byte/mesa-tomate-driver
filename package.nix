{ lib
, stdenv
, pkg-config
, libusb1
}:

stdenv.mkDerivation {
  pname = "mtm1106-mode";
  version = "0.1.0";

  src = ./.;

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ libusb1 ];

  doCheck = true;
  checkPhase = ''
    runHook preCheck
    ./mtm1106-mode --self-test
    runHook postCheck
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 mtm1106-mode $out/bin/mtm1106-mode
    runHook postInstall
  '';

  meta = {
    description = "USB mode activator for the MTM-1106 / T501 tablet";
    homepage = "https://github.com/Joaoferraz-byte/mesa-tomate-driver";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "mtm1106-mode";
  };
}
