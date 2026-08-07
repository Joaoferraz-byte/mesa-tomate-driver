# Protocolo de validação física

Este protocolo deve ser executado na máquina com a MTM-1106 conectada. A sessão do agente não teve acesso ao hardware físico; portanto, os resultados abaixo são critérios e comandos, não uma confirmação de funcionamento no dispositivo do usuário.

## 1. Estado inicial

Conecte a mesa sem executar `mtm1106-mode` e salve os dados do USB, do udev e do libinput. Crie um diretório fora do repositório para não commitar dados locais:

```bash
mkdir -p "$HOME/mtm1106-validation/before"
lsusb -v -d 08f2:6811 > "$HOME/mtm1106-validation/before/lsusb.txt"
udevadm info --export-db > "$HOME/mtm1106-validation/before/udev-db.txt"
libinput list-devices > "$HOME/mtm1106-validation/before/libinput-list.txt"
libinput debug-tablet > "$HOME/mtm1106-validation/before/libinput-tablet.txt"
```

Registre também `uname -a`, `nixos-version`, `ls -l /dev/hidraw*` e a saída de `journalctl -k -b | grep -Ei '08f2|6811|hid|usb'`. O diagnóstico fornecido antes da implementação indica que os intervalos esperados são aproximadamente `993x585 mm` para a região desktop e `205x137 mm` para a região mobile.

## 2. Primeiro experimento: perfil completo

Use o binário instalado pelo flake ou o binário de build local. Execute apenas uma vez:

```bash
sudo mtm1106-mode --profile digimend
```

O programa deve informar quatro `SET_REPORT` enviados, sem resetar a mesa. Se houver erro de permissão, desconecte a mesa, corrija a instalação/udev e reconecte; não repita a sequência em um dispositivo cujo estado não esteja claro.

Depois do envio, mova a caneta pelos limites físico grande e pequeno. Em seguida, desconecte e reconecte o cabo USB para forçar a reenumeração e capture o estado posterior:

```bash
mkdir -p "$HOME/mtm1106-validation/after-digimend"
lsusb -v -d 08f2:6811 > "$HOME/mtm1106-validation/after-digimend/lsusb.txt"
udevadm info --export-db > "$HOME/mtm1106-validation/after-digimend/udev-db.txt"
libinput list-devices > "$HOME/mtm1106-validation/after-digimend/libinput-list.txt"
libinput debug-tablet > "$HOME/mtm1106-validation/after-digimend/libinput-tablet.txt"
```

## 3. Critérios de aceitação

Considere o perfil confirmado somente se todos os critérios seguintes forem verdadeiros:

| Verificação | Resultado exigido |
| --- | --- |
| Identificação | O dispositivo continua sendo `08f2:6811`/`T501`; nenhuma outra mesa foi alterada. |
| Área | A região desktop grande é a fonte efetiva de coordenadas, aproximadamente `993x585 mm`; a região `205x137 mm` não é a única área ativa. |
| Resolução | Não foi aplicada escala artificial em libinput, compositor ou `xinput` para simular o resultado. |
| Eventos | Pressão, proximidade, clique e os dois botões da caneta continuam operacionais. |
| Teclas | Os botões físicos continuam gerando eventos esperados ou, no mínimo, não foram perdidos. |
| Estabilidade | Desconectar/reconectar não deixa o kernel, libinput ou a sessão gráfica em estado quebrado. |

Uma mudança de proporção do cursor sem confirmação do intervalo e dos eventos é um **falso positivo**.

## 4. Perfil alternativo

Se o perfil `digimend` falhar, desligue e reconecte a mesa, salve um novo estado `before-mx002` e execute uma única vez:

```bash
sudo mtm1106-mode --profile mx002
```

Esse perfil envia somente `08 03 00 ff f0 00 ff f0`, a sequência mínima usada pelo driver Rust `mx002_linux_driver`. Ele não é uma calibração e não deve ser combinado com o perfil completo sem reconectar o dispositivo.

## 5. Teste automático no NixOS

Somente após validar manualmente, altere no host latitude:

```nix
services."mtm1106-mode" = {
  enable = true;
  profile = "digimend";
  autoStart = true;
};
```

Faça o rebuild e observe a unidade quando reconectar a mesa:

```bash
systemctl status mtm1106-mode.service
journalctl -u mtm1106-mode.service -b --no-pager
```

Se houver mais de uma mesa `08f2:6811`, não habilite o modo automático: o helper recusa selecionar entre múltiplos dispositivos sem `--bus`/`--address`, e a unidade udev não deve ser usada nesse cenário.

## 6. Rollback

Para voltar ao comportamento original, defina `autoStart = false` ou `enable = false`, faça o rebuild e desconecte/reconecte a mesa. O projeto não grava firmware nem altera descritores persistentes. Se o comportamento ficar inconsistente durante a sessão, remova a alimentação USB antes de qualquer novo experimento.

## 7. Evidência a enviar para investigação

Se o modo não for ativado, envie somente os arquivos de texto dos diretórios `before`, `after-digimend` e, se aplicável, `after-mx002`, além de `journalctl` filtrado. Não envie dados pessoais nem o banco udev completo se ele contiver outros dispositivos; prefira extrair as linhas do caminho USB `08f2:6811`.
