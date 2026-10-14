#!/bin/sh
# SF19A2890 (2 cores x 2 VPEs; cpu0/1 = core 0, cpu2/3 = core 1).
#
# The generic packet-steering.uc places RPS by looking for NAPI threads.
# The WiFi driver (sf16a18_fmac) RXes from a tasklet and runs GRO in its
# own NAPI on the other core, so the generic script ends up steering each
# netdev onto its own IRQ CPU, i.e. no RPS at all (HB RX then saturated
# one VPE at ~110Mbit/s). Layout used here:
#   WiFi: RX tasklet on the IRQ CPU, GRO on the other core (driver),
#         RPS to the IRQ CPU's sibling VPE.
#   eth0: GRO in NAPI on the IRQ CPU, eth0 RPS (DSA untag) to the first
#         VPE of the other core, then the DSA user ports' RPS (IP/TCP,
#         flowtable, transmit to WiFi) to the IRQ CPU's sibling VPE. eth0
#         frames are DSA tagged and hash alike, so a wider mask spreads
#         nothing. The other core already takes both WiFi IRQs; with the
#         user ports there too, the 5GHz IRQ CPU carried WAN ingress, the
#         flowtable and the WiFi TX for every download (WAN to 5GHz client
#         through the flowtable 210 -> 300 Mbit/s; router-terminated TCP
#         down unchanged).

packet_steering="$1"
[ -n "$packet_steering" ] || packet_steering="$(uci -q get network.@globals[0].packet_steering)"

set_rps() {
	for q in /sys/class/net/"$1"/queues/rx-*/rps_cpus; do
		[ -e "$q" ] && echo "$2" > "$q"
	done
}

# first CPU set in the IRQ's affinity mask
irq_cpu() {
	local irq mask cpu=0
	irq="$(awk -v n="$1" '$NF == n { sub(":", "", $1); print $1; exit }' /proc/interrupts)"
	[ -n "$irq" ] || return 1
	mask=$((0x$(cat /proc/irq/"$irq"/smp_affinity)))
	while [ $cpu -lt 4 ] && [ $(( (mask >> cpu) & 1 )) -eq 0 ]; do
		cpu=$((cpu + 1))
	done
	echo $((cpu & 3))
}

for dev in /sys/class/net/*; do
	dev="${dev##*/}"
	[ "$packet_steering" = 0 ] && { set_rps "$dev" 0; continue; }
	case "$(readlink /sys/class/net/"$dev"/device 2>/dev/null)" in
	*wlan-lb) irq=umac-lb ;;
	*wlan-hb) irq=umac-hb ;;
	*) continue ;;
	esac
	cpu="$(irq_cpu $irq)" || continue
	set_rps "$dev" "$(printf %x $((1 << (cpu ^ 1))))"
done

[ "$packet_steering" = 0 ] && exit 0
cpu="$(irq_cpu eth0)" || exit 0
cpu=$(((cpu ^ 2) & 3))
set_rps eth0 "$(printf %x $((1 << cpu)))"
eth0_idx="$(cat /sys/class/net/eth0/ifindex)"
for dev in /sys/class/net/*; do
	dev="${dev##*/}"
	# DSA user ports: iflink is the conduit
	[ "$dev" != eth0 ] &&
		[ "$(cat /sys/class/net/"$dev"/iflink)" = "$eth0_idx" ] &&
		set_rps "$dev" "$(printf %x $((1 << (cpu ^ 3))))"
done
exit 0
