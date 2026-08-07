# MTM-1106 / T501 — ativador de área completa para Linux e NixOS

Este projeto fornece um **ativador USB de modo**, não um novo driver de kernel. Ele foi criado para a mesa comercialmente identificada como **MTM-1106**, que aparece no Linux como `SZ PENG YI LTD. [T501]` com VID/PID `08f2:6811`. O sintoma investigado é a exposição simultânea de uma região grande para desktop e de uma região menor para Android/mobile, com o Linux usando apenas a área menor por padrão.

A solução envia relatórios HID `SET_REPORT` documentados pela ferramenta [`DIGImend/10moons-tools`](https://github.com/DIGImend/10moons-tools). O perfil padrão `digimend` envia quatro relatórios de oito bytes à interface HID `2`; o perfil alternativo `mx002` envia somente o relatório central usado pelo projeto [`marvinbelfort/mx002_linux_driver`](https://github.com/marvinbelfort/mx002_linux_driver). O programa exige o VID/PID esperado, exige uma interface HID número `2`, não reseta a mesa e não modifica firmware ou descritores.

> **Estado de validação:** a sequência foi auditada contra implementações públicas para a família T501, mas ainda precisa ser executada e validada na MTM-1106 física do usuário. Não trate uma mudança no cursor como prova de sucesso; use os critérios de medição abaixo.

## O que foi descoberto

O arquivo `libinput.txt` fornecido pelo usuário mostra duas interfaces tablet do mesmo USB ID: uma com aproximadamente `993x585 mm` e outra com `205x137 mm`. O README do driver Windows extraído menciona separadamente `Android Mode` e `Work mode`, mas não expõe os bytes de comunicação. A ferramenta DigiMend e os drivers Linux públicos fornecem a evidência de protocolo necessária.

A sequência completa do perfil padrão é a seguinte:

| Ordem | Dados do relatório | Interpretação operacional |
| --- | --- | --- |
| 1 | `08 04 1d 01 ff ff 06 2e` | Relatório de seleção/descoberta da família T501 |
| 2 | `08 03 00 ff f0 00 ff f0` | Relatório associado à área completa pelo driver mx002 |
| 3 | `08 06 01 00 00 00 00 00` | Etapa intermediária da sequência DigiMend |
| 4 | `08 03 00 ff f0 00 ff f0` | Reaplicação do relatório de área completa |

Todos usam `bmRequestType=0x21`, `bRequest=0x09`, `wValue=0x0308`, `wIndex=2` e timeout de 250 ms. A atribuição semântica dos bytes é parcialmente inferida; o que está confirmado é a sequência observada em código público e a associação do relatório de 8 bytes a uma mudança de modo.

## Uso seguro antes do NixOS

O roteiro completo de captura antes/depois e os critérios de aceitação está em [`TESTING.md`](./TESTING.md). Use-o antes de habilitar `autoStart`.

Compile e execute os testes locais:

```bash
make check
```

O self-test não abre USB. Para apenas visualizar os bytes:

```bash
./mtm1106-mode --profile digimend --dry-run
./mtm1106-mode --profile mx002 --dry-run
```

Antes de abrir a mesa, registre o estado inicial. Em um sistema com `libinput`, salve pelo menos:

```bash
lsusb -v -d 08f2:6811 > before-lsusb.txt
libinput debug-tablet > before-libinput.txt
udevadm info --export-db > before-udev.txt
```

O comando normal deve ser executado com a mesa conectada e com privilégios suficientes para reivindicar a interface HID:

```bash
sudo ./mtm1106-mode --profile digimend
```

Se a sequência completa não produzir o resultado esperado, desligue e reconecte a mesa antes de qualquer segundo experimento. Depois compare os mesmos diagnósticos. O perfil `mx002` é uma alternativa, não uma calibração: não o execute repetidamente em sequência sem reconectar o dispositivo.

A confirmação mínima exige que a área grande volte a ser a fonte efetiva de coordenadas, aproximadamente `993x585 mm`, e que a área `205x137 mm` deixe de ser a única interface tablet usada. Também devem ser testados pressão, os dois botões da caneta, teclas da mesa e reconexão.

## Integração NixOS

O flake exporta o pacote e o módulo `nixosModules.default`. No `flake.nix` do seu `nix-conf`, adicione a entrada:

```nix
inputs.mesa-tomate-driver.url = "github:Joaoferraz-byte/mesa-tomate-driver";
```

No host que possui a mesa, importe o módulo:

```nix
self.nixosModules.mtm1106 = inputs.mesa-tomate-driver.nixosModules.default;
```

Em seguida, inclua `self.nixosModules.mtm1106` na lista `imports` de `latitudeConfiguration` ou do host correto e mantenha a ativação automática desligada inicialmente:

```nix
services."mtm1106-mode" = {
  enable = true;
  profile = "digimend";
  autoStart = false;
};
```

Isso instala `mtm1106-mode` no sistema, mas exige execução manual. O primeiro teste após o rebuild é:

```bash
sudo mtm1106-mode --profile digimend
```

Só depois de confirmar repetidamente o comportamento físico deve ser habilitado:

```nix
services."mtm1106-mode" = {
  enable = true;
  profile = "digimend";
  autoStart = true;
};
```

Com `autoStart`, uma regra udev dispara uma unidade oneshot para `08f2:6811`. O helper se recusa a agir se não encontrar a interface HID `2` ou se houver mais de uma mesa compatível sem seleção por barramento/endereço. A unidade usa `NoNewPrivileges`, `PrivateTmp`, `ProtectHome` e `ProtectSystem`; a execução continua sendo root apenas porque a interface HID pode estar sob controle de `hid-generic`.

## Rollback

Para desativar sem alterar o restante do sistema, remova ou defina `enable = false` em `services."mtm1106-mode"`, retire o input do flake se desejar e faça um rebuild. A solução não instala módulo de kernel e não grava firmware. Se uma sessão gráfica ficar sem a mesa após um experimento, desconecte e reconecte o USB; o ativador não executa `reset` e não deve persistir uma modificação depois que a energia é removida.

## Limitações e falsos positivos

O VID/PID é compartilhado por rebrands do chipset T501. Por isso o projeto não afirma que todo `08f2:6811` é uma MTM-1106, embora exija adicionalmente a interface HID `2`. A sequência não possui uma resposta de leitura padronizada que permita declarar sucesso automaticamente. A dimensão mostrada pelo libinput pode permanecer igual se a mesa não for reenumerada, se o compositor estiver usando uma interface antiga ou se o comando tiver sido enviado à interface errada.

O projeto não corrige pressão, rotação, mapeamento para múltiplos monitores ou a qualidade do parser de eventos. Essas são camadas posteriores. Primeiro confirme o modo USB; só depois ajuste libinput, Niri ou um driver de usuário. Não adicione uma transformação de escala para esconder uma área errada, porque isso produziria um falso positivo visual e perderia resolução física.

## Referências

[1]: https://docs.kernel.org/hid/hidintro.html "Linux Kernel: Introduction to HID report descriptors"
[2]: https://github.com/DIGImend/10moons-tools "DIGImend 10moons-tools"
[3]: https://github.com/marvinbelfort/mx002_linux_driver "mx002 Linux user-space driver"
[4]: https://github.com/f-caro/10moons-driver-vin1060plus "10moons/VINSA T501 reference driver"
[5]: https://bbs.archlinux.org/viewtopic.php?id=308509 "Arch Linux: Graphic tablet cuts its work area [SOLVED]"
[6]: https://github.com/DIGImend/digimend-kernel-drivers/issues/722 "DIGImend issue #722: MTM-1106 / T501"
[7]: https://github.com/Joaoferraz-byte/nix-conf "User NixOS configuration"
