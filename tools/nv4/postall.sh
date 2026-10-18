#!/bin/bash
# postall.sh -- POST every RIVA TNT/TNT2 model on a disk-less VM and keep
# a screenshot of each video BIOS banner and of the BIOS screen after it.
#   TNT_DIR=<vm> postall.sh [device:model ...]
# The VM's 86box.cfg.orig is the template (its [Video] gfxcard line is
# replaced); 86box.cfg is restored from it at the end.
T="bash $(dirname "$0")/tnt.sh"
W=${TNT_DIR:?set TNT_DIR}
CFG="$W/86box.cfg"
[ -f "$CFG.orig" ] || cp "$CFG" "$CFG.orig"
declare -A NAME=(
    [nv4_tnt_pci]="nVIDIA RIVA TNT (NV4) PCI"
    [nv4_tnt_agp]="nVIDIA RIVA TNT (NV4) AGP"
    [nv5_tnt2_pci]="nVIDIA RIVA TNT2 (NV5) PCI"
    [nv5_tnt2_agp]="nVIDIA RIVA TNT2 (NV5) AGP"
    [nv5_tnt2_ultra_pci]="nVIDIA RIVA TNT2 Ultra (NV5) PCI"
    [nv5_tnt2_ultra_agp]="nVIDIA RIVA TNT2 Ultra (NV5) AGP"
)
LIST=${*:-"nv4_tnt_pci:nv4_erazor2_pci nv4_tnt_agp:nv4_erazor2_agp_203 nv4_tnt_agp:nv4_erazor2_agp
nv4_tnt_agp:nv4_viper_v550_193 nv4_tnt_agp:nv4_viper_v550_195e nv4_tnt_agp:nv4_viper_v550
nv4_tnt_agp:nv4_creative_tnt nv4_tnt_agp:nv4_velocity_4400_101 nv4_tnt_agp:nv4_velocity_4400
nv4_tnt_agp:nv4_asus_v3400tnt nv5_tnt2_agp:nv5_erazor3 nv5_tnt2_agp:nv5_erazor3_pro
nv5_tnt2_agp:nv5_erazor3_pro_oem nv5_tnt2_agp:nv5_erazor3_lt nv5_tnt2_pci:nv5_synergy2_32
nv5_tnt2_agp:nv5_viper_v770 nv5_tnt2_agp:nv5_winfast_s320ii nv5_tnt2_ultra_agp:nv5_erazor3_ultra"}
for item in $LIST; do
    dev=${item%%:*}
    model=${item#*:}
    # the template without its video sections, then this card
    sed -e "s/^gfxcard = .*/gfxcard = $dev/" "$CFG.orig" |
        awk '/^\[/{skip = ($0 ~ /RIVA|ERAZOR|Viper|Velocity|WinFast|Graphics Blaster|V3400/)} !skip' > "$CFG"
    printf '\n[%s #1]\nbios = %s\n' "${NAME[$dev]}" "$model" >> "$CFG"
    $T start
    sleep 5
    $T shot "p_${model}_a"
    sleep 9
    $T shot "p_${model}_b"
    grep -h "nv4: nVIDIA" "$W/86box.log" | tail -1
    $T stop
    for i in $(seq 1 30); do [ "$($T running)" = "0" ] && break; sleep 0.5; done
done
cp "$CFG.orig" "$CFG"
