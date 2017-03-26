
create_netns()
{
	local netns=$1
	ip netns add ${netns}
	ip netns exec ${netns} ip link set lo up
}

link_netns()
{
	local netns1=$1
	local netns1_itf=$2
	local netns2=$3
	local netns2_itf=$4
	ip link add name ${netns1_itf} type veth peer name ${netns2_itf}
	ip link set ${netns1_itf} netns ${netns1}
	ip netns exec ${netns1} ip link set ${netns1_itf} up
	ip link set ${netns2_itf} netns ${netns2}
	ip netns exec ${netns2} ip link set ${netns2_itf} up
}

for netns in CUSTOMER SNT SNP SERVER ; do
	ip netns del ${netns}
	create_netns ${netns}
done
unset netns

link_netns  CUSTOMER internet   SNT lan
link_netns  SNT      dvbsat     SNP satellite
link_netns  SNP      tiss       SERVER wan

ip netns exec CUSTOMER ip -4 addr add 192.168.0.1/24 dev internet
ip netns exec CUSTOMER ip route add default dev internet
ip netns exec SERVER   ip -4 addr add 192.168.1.254/24 dev wan
ip netns exec SERVER   ip route add default dev wan

#MAC_SERVER=$( ip netns exec SERVER   ip link show dev wan | tail -n 1 | gawk '{ print $2 }' )
#ip netns exec CUSTOMER ip neigh add 192.168.1.254 lladdr ${MAC_SERVER} dev internet
#MAC_CLIENT=$( ip netns exec CUSTOMER   ip link show dev internet | tail -n 1 | gawk '{ print $2 }' )
#ip netns exec SERVER ip neigh add 192.168.0.1 lladdr ${MAC_CLIENT} dev wan

ip netns exec SERVER ethtool -K wan tx off
ip netns exec SERVER ethtool -K wan rx off
ip netns exec CUSTOMER ethtool -K internet rx off
ip netns exec CUSTOMER ethtool -K internet tx off
