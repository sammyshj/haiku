/*
 * Copyright 2006-2010, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de
 *		Oliver Tappe, zooey@hirschkaefer.de
 *		Atis Elsts, the.kfx@gmail.com
 */


#include <errno.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Message.h>
#include <Messenger.h>
#include <NetworkDevice.h>
#include <NetworkInterface.h>
#include <NetworkRoster.h>

#include <NetServer.h>


extern const char* __progname;
const char* kProgramName = __progname;


enum preferred_output_format {
	PREFER_OUTPUT_MASK,
	PREFER_OUTPUT_PREFIX_LENGTH,
};

struct address_family {
	int			family;
	const char*	name;
	const char*	identifiers[4];
	preferred_output_format	preferred_format;
};

struct media_type {
	int			type;
	const char*	name;
	const char* pretty;
	struct {
		int subtype;
		const char* name;
		const char* pretty;
	} subtypes [6];
	struct {
		int option;
		bool read_only;
		const char* name;
		const char* pretty;
	} options [6];
};


static const address_family kFamilies[] = {
	{
		AF_INET,
		"inet",
		{"AF_INET", "inet", "ipv4", NULL},
		PREFER_OUTPUT_MASK
	},
	{
		AF_INET6,
		"inet6",
		{"AF_INET6", "inet6", "ipv6", NULL},
		PREFER_OUTPUT_PREFIX_LENGTH
	},
	{ -1, NULL, {NULL}, PREFER_OUTPUT_MASK }
};

static const media_type kMediaTypes[] = {
	{
		0, // for generic options
		"all",
		"All",
		{
			{ IFM_AUTO, "auto", "Auto-select" },
			{ -1, NULL, NULL }
		},
		{
			{ IFM_FULL_DUPLEX, true, "fullduplex", "Full Duplex" },
			{ IFM_HALF_DUPLEX, true, "halfduplex", "Half Duplex" },
			{ IFM_LOOP, true, "loop", "Loop" },
			//{ IFM_ACTIVE, false, "active", "Active" },
			{ -1, false, NULL, NULL }
		}
	},
	{
		IFM_ETHER,
		"ether",
		"Ethernet",
		{
			//{ IFM_AUTO, "auto", "Auto-select" },
			//{ IFM_AUI, "AUI", "10 MBit, AUI" },
			//{ IFM_10_2, "10base2", "10 MBit, 10BASE-2" },
			{ IFM_10_T, "10baseT", "10 MBit, 10BASE-T" },
			{ IFM_100_TX, "100baseTX", "100 MBit, 100BASE-TX" },
			{ IFM_1000_T, "1000baseT", "1 GBit, 1000BASE-T" },
			{ IFM_1000_SX, "1000baseSX", "1 GBit, 1000BASE-SX" },
			{ IFM_10G_T, "10GbaseT", "10 GBit, 10GBASE-T" },
			{ -1, NULL, NULL }
		},
		{
			{ -1, false, NULL, NULL }
		}
	},
	{ -1, NULL, NULL, {{ -1, NULL, NULL }}, {{ -1, false, NULL, NULL }} }
};


static void
usage(int status)
{
	printf("usage: %s [<interface> [<address family>] [<address> [<mask>] | "
			"auto-config] [<option/flags>...]]\n"
		"       %s --delete <interface> [...]\n"
		"       %s <interface> [scan|join|leave] [<network> "
			"[<password>]]\n\n"
		"Where <option> can be the following:\n"
		"  netmask <addr>     - networking subnet mask\n"
		"  prefixlen <number> - subnet mask length in bits\n"
		"  broadcast <addr>   - set broadcast address\n"
		"  peer <addr>        - ppp-peer address\n"
		"  mtu <bytes>        - maximal transfer unit\n"
		"  metric <number>    - metric number to use (defaults to 0)\n"
		"  media <media>      - media type to use (defaults to auto)\n",
		kProgramName, kProgramName, kProgramName);

	for (int32 i = 0; kMediaTypes[i].type >= 0; i++) {
		printf("For %s <media> can be one of: ", kMediaTypes[i].pretty);
		for (int32 j = 0; kMediaTypes[i].subtypes[j].subtype >= 0; j++)
			printf("%s ", kMediaTypes[i].subtypes[j].name);
		printf("\n");
	}
	printf("And <flags> can be: up, down, [-]promisc, [-]allmulti, [-]bcast, "
			"loopback\n"
		"If you specify \"auto-config\" instead of an address, it will be "
			"configured automatically.\n\n"
		"Example:\n"
		"\t%s loop 127.0.0.1 255.0.0.0 up\n",
		kProgramName);

	exit(status);
}


static bool
media_parse_subtype(const char* string, int media, int* type)
{
	for (int32 i = 0; kMediaTypes[i].type >= 0; i++) {
		// only check for generic or correct subtypes
		if (kMediaTypes[i].type &&
			kMediaTypes[i].type != media)
			continue;
		for (int32 j = 0; kMediaTypes[i].subtypes[j].subtype >= 0; j++) {
			if (strcmp(kMediaTypes[i].subtypes[j].name, string) == 0) {
				// found a match
				*type = kMediaTypes[i].subtypes[j].subtype;
				return true;
			}
		}
	}
	return false;
}


int
get_address_family(const char* argument)
{
	for (int32 i = 0; kFamilies[i].family >= 0; i++) {
		for (int32 j = 0; kFamilies[i].identifiers[j]; j++) {
			if (!strcmp(argument, kFamilies[i].identifiers[j])) {
				// found a match
				return kFamilies[i].family;
			}
		}
	}

	return AF_UNSPEC;
}


static const address_family*
address_family_for(int family)
{
	for (int32 i = 0; kFamilies[i].family >= 0; i++) {
		if (kFamilies[i].family == family)
			return &kFamilies[i];
	}

	// defaults to AF_INET
	return &kFamilies[0];
}


/*!	Parses the \a argument as network \a address for the specified \a family.
	If \a family is \c AF_UNSPEC, \a family will be overwritten with the family
	of the successfully parsed address.
*/
bool
parse_address(int& family, const char* argument, BNetworkAddress& address)
{
	if (argument == NULL)
		return false;

	status_t status = address.SetTo(family, argument, (uint16)0,
		B_NO_ADDRESS_RESOLUTION);
	if (status != B_OK)
		return false;

	if (family == AF_UNSPEC) {
		// Test if we support the resulting address family
		bool supported = false;

		for (int32 i = 0; kFamilies[i].family >= 0; i++) {
			if (kFamilies[i].family == address.Family()) {
				supported = true;
				break;
			}
		}
		if (!supported)
			return false;

		// Take over family from address
		family = address.Family();
	}

	return true;
}


bool
prefix_length_to_mask(int family, const char* argument, BNetworkAddress& mask)
{
	char *end;
	uint32 prefixLength = strtoul(argument, &end, 10);
	if (end == argument)
		return false;

	return mask.SetToMask(family, prefixLength) == B_OK;
}


//	#pragma mark - wireless support


const char*
get_key_mode(uint32 mode)
{
	if ((mode & B_KEY_MODE_WPS) != 0)
		return "WPS";
	if ((mode & B_KEY_MODE_PSK_SHA256) != 0)
		return "PSK-SHA256";
	if ((mode & B_KEY_MODE_IEEE802_1X_SHA256) != 0)
		return "IEEE 802.1x-SHA256";
	if ((mode & B_KEY_MODE_FT_PSK) != 0)
		return "FT-PSK";
	if ((mode & B_KEY_MODE_FT_IEEE802_1X) != 0)
		return "FT-IEEE 802.1x";
	if ((mode & B_KEY_MODE_NONE) != 0)
		return "-";
	if ((mode & B_KEY_MODE_PSK) != 0)
		return "PSK";
	if ((mode & B_KEY_MODE_IEEE802_1X) != 0)
		return "IEEE 802.1x";

	return "";
}


const char*
get_cipher(uint32 cipher)
{
	if ((cipher & B_NETWORK_CIPHER_AES_128_CMAC) != 0)
		return "AES-128-CMAC";
	if ((cipher & B_NETWORK_CIPHER_CCMP) != 0)
		return "CCMP";
	if ((cipher & B_NETWORK_CIPHER_TKIP) != 0)
		return "TKIP";
	if ((cipher & B_NETWORK_CIPHER_WEP_104) != 0)
		return "WEP-104";
	if ((cipher & B_NETWORK_CIPHER_WEP_40) != 0)
		return "WEP-40";

	return "";
}


const char*
get_authentication_mode(uint32 mode, uint32 flags)
{
	switch (mode) {
		default:
		case B_NETWORK_AUTHENTICATION_NONE:
			if ((flags & B_NETWORK_IS_ENCRYPTED) != 0)
				return "(encrypted)";
			return "-";
		case B_NETWORK_AUTHENTICATION_WEP:
			return "WEP";
		case B_NETWORK_AUTHENTICATION_WPA:
			return "WPA";
		case B_NETWORK_AUTHENTICATION_WPA2:
			return "WPA2";
	}
}


void
show_wireless_network_header(bool verbose)
{
	printf("%-32s %-20s %s  %s\n", "name", "address", "signal", "auth");
}


void
show_wireless_network(const wireless_network& network, bool verbose)
{
	printf("%-32s %-20s %6u  %s\n", network.name,
		network.address.ToString().String(),
		network.signal_strength / 2,
		get_authentication_mode(network.authentication_mode, network.flags));
}


bool
configure_wireless(const char* name, char* const* args, int32 argCount)
{
	enum {
		NONE,
		LIST,
		JOIN,
		LEAVE
	} mode = NONE;

	if (!strcmp(args[0], "list") || !strcmp(args[0], "scan"))
		mode = LIST;
	else if (!strcmp(args[0], "join"))
		mode = JOIN;
	else if (!strcmp(args[0], "leave"))
		mode = LEAVE;

	if (mode == NONE)
		return false;

	BNetworkDevice device(name);
	if (!device.Exists()) {
		fprintf(stderr, "%s: \"%s\" does not exist!\n", kProgramName, name);
		exit(1);
	}
	if (!device.IsWireless()) {
		fprintf(stderr, "%s: \"%s\" is not a WLAN device!\n", kProgramName,
			name);
		exit(1);
	}

	args++;
	argCount--;

	switch (mode) {
		case LIST:
		{
			// list wireless network(s)

			bool verbose = false;
			if (argCount > 0 && !strcmp(args[0], "-v")) {
				verbose = true;
				args++;
				argCount--;
			}
			show_wireless_network_header(verbose);

			if (argCount > 0) {
				// list the named entries
				for (int32 i = 0; i < argCount; i++) {
					wireless_network network;
					BNetworkAddress link;
					status_t status;
					if (link.SetTo(AF_LINK, args[i]) == B_OK)
						status = device.GetNetwork(link, network);
					else
						status = device.GetNetwork(args[i], network);
					if (status != B_OK) {
						fprintf(stderr, "%s: Getting network failed: %s\n",
							kProgramName, strerror(status));
					} else
						show_wireless_network(network, verbose);
				}
			} else {
				// list them all
				wireless_network network;
				uint32 cookie = 0;
				while (device.GetNextNetwork(cookie, network) == B_OK)
					show_wireless_network(network, verbose);
			}
			break;
		}

		case JOIN:
		{
			// join a wireless network
			if (argCount > 2) {
				fprintf(stderr, "usage: %s %s join <network> [<password>]\n",
					kProgramName, name);
				exit(1);
			}

			const char* password = NULL;
			if (argCount == 2)
				password = args[1];

			BNetworkAddress link;
			status_t status;
			if (link.SetTo(AF_LINK, args[0]) == B_OK)
				status = device.JoinNetwork(link, password);
			else
				status = device.JoinNetwork(args[0], password);
			if (status != B_OK) {
				fprintf(stderr, "%s: Joining network failed: %s\n",
					kProgramName, strerror(status));
				exit(1);
			}
			break;
		}

		case LEAVE:
		{
			// leave a wireless network
			if (argCount != 1) {
				fprintf(stderr, "usage: %s %s leave <network>\n", kProgramName,
					name);
				exit(1);
			}

			BNetworkAddress link;
			status_t status;
			if (link.SetTo(AF_LINK, args[0]) == B_OK)
				status = device.LeaveNetwork(link);
			else
				status = device.LeaveNetwork(args[0]);
			if (status != B_OK) {
				fprintf(stderr, "%s: Leaving network failed: %s\n",
					kProgramName, strerror(status));
				exit(1);
			}
			break;
		}

		case NONE:
			break;
	}

	return true;
}


//	#pragma mark -


void
list_interface_addresses(BNetworkInterface& interface, uint32 flags)
{
	int32 count = interface.CountAddresses();
	for (int32 i = 0; i < count; i++) {
		BNetworkInterfaceAddress address;
		if (interface.GetAddressAt(i, address) != B_OK)
			break;

		const address_family* family
			= address_family_for(address.Address().sa_family);

		printf("\t%s addr: %s", family->name,
			BNetworkAddress(address.Address()).ToString().String());

		if ((flags & IFF_BROADCAST) != 0) {
			printf(", Bcast: %s",
				BNetworkAddress(address.Broadcast()).ToString().String());
		}

		switch (family->preferred_format) {
			case PREFER_OUTPUT_MASK:
				printf(", Mask: %s",
					BNetworkAddress(address.Mask()).ToString().String());
				break;
			case PREFER_OUTPUT_PREFIX_LENGTH:
				printf(", Prefix Length: %zu",
					BNetworkAddress(address.Mask()).PrefixLength());
				break;
		}

		putchar('\n');
	}
}


bool
list_interface(const char* name)
{
	printf("%s", name);
	size_t length = strlen(name);
	if (length < 8)
		putchar('\t');
	else
		printf("\n\t");

	// get link level interface for this interface

	BNetworkInterface interface(name);
	if (!interface.Exists()) {
		printf("Interface not found!\n");
		return false;
	}

	BNetworkAddress linkAddress;
	status_t status = interface.GetHardwareAddress(linkAddress);
	if (status == B_OK) {
		const char *type = "unknown";
		switch (linkAddress.LinkLevelType()) {
			case IFT_ETHER:
				type = "Ethernet";
				break;
			case IFT_LOOP:
				type = "Local Loopback";
				break;
			case IFT_MODEM:
				type = "Modem";
				break;
		}

		BString address = linkAddress.ToString();
		if (address.Length() == 0)
			address = "none";

		printf("Hardware type: %s, Address: %s\n", type, address.String());
	} else
		printf("No link level: %s\n", strerror(status));

	int media = interface.Media();
	if ((media & IFM_ACTIVE) != 0) {
		// dump media state in case we're linked
		const char* type = "unknown";
		bool show = false;

		for (int32 i = 0; kMediaTypes[i].type >= 0; i++) {
			// loopback don't really have a media anyway
			if (IFM_TYPE(media) == 0/*IFT_LOOP*/)
				break;

			// only check for generic or correct subtypes
			if (kMediaTypes[i].type
				&& kMediaTypes[i].type != IFM_TYPE(media))
				continue;

			for (int32 j = 0; kMediaTypes[i].subtypes[j].subtype >= 0; j++) {
				if (kMediaTypes[i].subtypes[j].subtype == IFM_SUBTYPE(media)) {
					// found a match
					type = kMediaTypes[i].subtypes[j].pretty;
					show = true;
					break;
				}
			}
		}

		if (show)
			printf("\tMedia type: %s\n", type);
	}

	// Print associated wireless network(s)

	BNetworkDevice device(name);
	if (device.IsWireless()) {
		wireless_network network;
		bool first = true;
		uint32 cookie = 0;
		while (device.GetNextAssociatedNetwork(cookie, network) == B_OK) {
			if (first) {
				printf("\tNetwork: ");
				first = false;
			} else
				printf("\t\t");

			printf("%s, Address: %s, %s", network.name,
				network.address.ToString().String(),
				get_authentication_mode(network.authentication_mode,
					network.flags));
			const char* keyMode = get_key_mode(network.key_mode);
			if (keyMode != NULL)
				printf(", %s/%s", keyMode, get_cipher(network.cipher));
			putchar('\n');
		}
	}

	uint32 flags = interface.Flags();

	list_interface_addresses(interface, flags);

	// Print MTU, metric, flags

	printf("\tMTU: %" B_PRId32 ", Metric: %" B_PRId32, interface.MTU(),
		interface.Metric());

	if (flags != 0) {
		const struct {
			int			value;
			const char	*name;
		} kFlags[] = {
			{IFF_UP, "up"},
			{IFF_NOARP, "noarp"},
			{IFF_BROADCAST, "broadcast"},
			{IFF_LOOPBACK, "loopback"},
			{IFF_PROMISC, "promiscuous"},
			{IFF_ALLMULTI, "allmulti"},
			{IFF_AUTOUP, "autoup"},
			{IFF_LINK, "link"},
			{IFF_AUTO_CONFIGURED, "auto-configured"},
			{IFF_CONFIGURING, "configuring"},
		};
		bool first = true;

		for (uint32 i = 0; i < sizeof(kFlags) / sizeof(kFlags[0]); i++) {
			if ((flags & kFlags[i].value) != 0) {
				if (first) {
					printf(",");
					first = false;
				}
				putchar(' ');
				printf(kFlags[i].name);
			}
		}
	}

	putchar('\n');

	// Print statistics

	ifreq_stats stats;
	if (interface.GetStats(stats) == B_OK) {
		printf("\tReceive: %d packets, %d errors, %Ld bytes, %d mcasts, %d "
			"dropped\n", stats.receive.packets, stats.receive.errors,
			stats.receive.bytes, stats.receive.multicast_packets,
			stats.receive.dropped);
		printf("\tTransmit: %d packets, %d errors, %Ld bytes, %d mcasts, %d "
			"dropped\n", stats.send.packets, stats.send.errors,
			stats.send.bytes, stats.send.multicast_packets, stats.send.dropped);
		printf("\tCollisions: %d\n", stats.collisions);
	}

	putchar('\n');
	return true;
}


void
list_interfaces(const char* name)
{
	if (name != NULL) {
		list_interface(name);
		return;
	}

	// get a list of all interfaces

	BNetworkRoster& roster = BNetworkRoster::Default();

	BNetworkInterface interface;
	uint32 cookie = 0;

	while (roster.GetNextInterface(&cookie, interface) == B_OK) {
		list_interface(interface.Name());
	}
}


/*!	If there are any arguments given, this will remove only the specified
	addresses from the interface named \a name.
	If there are no arguments, it will remove the complete interface with all
	of its addresses.
*/
void
delete_interface(const char* name, char* const* args, int32 argCount)
{
	BNetworkInterface interface(name);

	for (int32 i = 0; i < argCount; i++) {
		int family = get_address_family(args[i]);
		if (family != AF_UNSPEC)
			i++;

		BNetworkAddress address;
		if (!parse_address(family, args[i], address)) {
			fprintf(stderr, "%s: Could not parse address \"%s\".\n",
				kProgramName, args[i]);
			exit(1);
		}

		status_t status = interface.RemoveAddress(address);
		if (status != B_OK) {
			fprintf(stderr, "%s: Could not delete address %s from interface %s:"
				" %s\n", kProgramName, args[i], name, strerror(status));
		}
	}

	if (argCount == 0) {
		// Delete interface
		BNetworkRoster& roster = BNetworkRoster::Default();

		status_t status = roster.RemoveInterface(interface);
		if (status != B_OK) {
			fprintf(stderr, "%s: Could not delete interface %s: %s\n",
				kProgramName, name, strerror(errno));
		}
	}
}


void
configure_interface(const char* name, char* const* args, int32 argCount)
{
	// try to parse address family

	int32 i = 0;
	int family = get_address_family(args[i]);
	if (family != AF_UNSPEC)
		i++;

	// try to parse address

	BNetworkAddress address;
	BNetworkAddress mask;

	if (parse_address(family, args[i], address)) {
		i++;

		if (parse_address(family, args[i], mask))
			i++;
	}

	BNetworkInterface interface(name);
	if (!interface.Exists()) {
		// the interface does not exist yet, we have to add it first
		BNetworkRoster& roster = BNetworkRoster::Default();

		status_t status = roster.AddInterface(interface);
		if (status != B_OK) {
			fprintf(stderr, "%s: Could not add interface: %s\n", kProgramName,
				strerror(status));
			exit(1);
		}
	}

	BNetworkAddress broadcast;
	BNetworkAddress peer;
	int mtu = -1, metric = -1, media = -1;
	int addFlags = 0, currentFlags = 0, removeFlags = 0;
	bool doAutoConfig = false;

	// parse parameters and flags

	while (i < argCount) {
		if (!strcmp(args[i], "peer")) {
			if (!parse_address(family, args[i + 1], peer)) {
				fprintf(stderr, "%s: Option 'peer' needs valid address "
					"parameter\n", kProgramName);
				exit(1);
			}
			i++;
		} else if (!strcmp(args[i], "nm") || !strcmp(args[i], "netmask")) {
			if (!mask.IsEmpty()) {
				fprintf(stderr, "%s: Netmask or prefix length is specified "
					"twice\n", kProgramName);
				exit(1);
			}
			if (!parse_address(family, args[i + 1], mask)) {
				fprintf(stderr, "%s: Option 'netmask' needs valid address "
					"parameter\n", kProgramName);
				exit(1);
			}
			i++;
		} else if (!strcmp(args[i], "prefixlen") || !strcmp(args[i], "plen")
			|| !strcmp(args[i], "prefix-length")) {
			if (!mask.IsEmpty()) {
				fprintf(stderr, "%s: Netmask or prefix length is specified "
					"twice\n", kProgramName);
				exit(1);
			}

			// default to AF_INET if no address family has been specified yet
			if (family == AF_UNSPEC)
				family = AF_INET;

			if (!prefix_length_to_mask(family, args[i + 1], mask)) {
				fprintf(stderr, "%s: Option 'prefix-length %s' is invalid for "
					"this address family\n", kProgramName, args[i + 1]);
				exit(1);
			}
			i++;
		} else if (!strcmp(args[i], "bc") || !strcmp(args[i], "broadcast")) {
			if (!broadcast.IsEmpty()) {
				fprintf(stderr, "%s: broadcast address is specified twice\n",
					kProgramName);
				exit(1);
			}
			if (!parse_address(family, args[i + 1], broadcast)) {
				fprintf(stderr, "%s: Option 'broadcast' needs valid address "
					"parameter\n", kProgramName);
				exit(1);
			}
			addFlags |= IFF_BROADCAST;
			i++;
		} else if (!strcmp(args[i], "mtu")) {
			mtu = args[i + 1] ? strtol(args[i + 1], NULL, 0) : 0;
			if (mtu <= 500) {
				fprintf(stderr, "%s: Option 'mtu' expected valid max transfer "
					"unit size\n", kProgramName);
				exit(1);
			}
			i++;
		} else if (!strcmp(args[i], "metric")) {
			if (i + 1 >= argCount) {
				fprintf(stderr, "%s: Option 'metric' expected parameter\n",
					kProgramName);
				exit(1);
			}
			metric = strtol(args[i + 1], NULL, 0);
			i++;
		} else if (!strcmp(args[i], "media")) {
			media = interface.Media();
			if (media < 0) {
				fprintf(stderr, "%s: Unable to detect media type\n",
					kProgramName);
				exit(1);
			}
			if (i + 1 >= argCount) {
				fprintf(stderr, "%s: Option 'media' expected parameter\n",
					kProgramName);
				exit(1);
			}
			if (!media_parse_subtype(args[i + 1], IFM_TYPE(media), &media)) {
				fprintf(stderr, "%s: Invalid parameter for option 'media': "
					"'%s'\n", kProgramName, args[i + 1]);
				exit(1);
			}
			i++;
		} else if (!strcmp(args[i], "up") || !strcmp(args[i], "-down")) {
			addFlags |= IFF_UP;
		} else if (!strcmp(args[i], "down") || !strcmp(args[i], "-up")) {
			removeFlags |= IFF_UP;
		} else if (!strcmp(args[i], "bcast")) {
			addFlags |= IFF_BROADCAST;
		} else if (!strcmp(args[i], "-bcast")) {
			removeFlags |= IFF_BROADCAST;
		} else if (!strcmp(args[i], "promisc")) {
			addFlags |= IFF_PROMISC;
		} else if (!strcmp(args[i], "-promisc")) {
			removeFlags |= IFF_PROMISC;
		} else if (!strcmp(args[i], "allmulti")) {
			addFlags |= IFF_ALLMULTI;
		} else if (!strcmp(args[i], "-allmulti")) {
			removeFlags |= IFF_ALLMULTI;
		} else if (!strcmp(args[i], "loopback")) {
			addFlags |= IFF_LOOPBACK;
		} else if (!strcmp(args[i], "auto-config")) {
			doAutoConfig = true;
		} else
			usage(1);

		i++;
	}

	if ((addFlags & removeFlags) != 0) {
		fprintf(stderr, "%s: Contradicting flags specified\n", kProgramName);
		exit(1);
	}

	if (doAutoConfig && (!address.IsEmpty() || !mask.IsEmpty()
			|| !broadcast.IsEmpty() || !peer.IsEmpty())) {
		fprintf(stderr, "%s: Contradicting changes specified\n", kProgramName);
		exit(1);
	}

	// set address/mask/broadcast/peer

	if (!address.IsEmpty() || !mask.IsEmpty() || !broadcast.IsEmpty()) {
		BNetworkInterfaceAddress interfaceAddress;
		interfaceAddress.SetAddress(address);
		interfaceAddress.SetMask(mask);
		if (!broadcast.IsEmpty())
			interfaceAddress.SetBroadcast(broadcast);
		else if (!peer.IsEmpty())
			interfaceAddress.SetDestination(peer);

		status_t status = interface.SetAddress(interfaceAddress);
		if (status != B_OK) {
			fprintf(stderr, "%s: Setting address failed: %s\n", kProgramName,
				strerror(status));
			exit(1);
		}
	}

	currentFlags = interface.Flags();

	// set flags

	if (!address.IsEmpty() || !mask.IsEmpty() || !broadcast.IsEmpty()
		|| !peer.IsEmpty())
		removeFlags = IFF_AUTO_CONFIGURED | IFF_CONFIGURING;

	if (addFlags || removeFlags) {
		status_t status
			= interface.SetFlags((currentFlags & ~removeFlags) | addFlags);
		if (status != B_OK) {
			fprintf(stderr, "%s: Setting flags failed: %s\n", kProgramName,
				strerror(status));
		}
	}

	// set options

	if (mtu != -1) {
		status_t status = interface.SetMTU(mtu);
		if (status != B_OK) {
			fprintf(stderr, "%s: Setting MTU failed: %s\n", kProgramName,
				strerror(status));
		}
	}

	if (metric != -1) {
		status_t status = interface.SetMetric(metric);
		if (status != B_OK) {
			fprintf(stderr, "%s: Setting metric failed: %s\n", kProgramName,
				strerror(status));
		}
	}

	if (media != -1) {
		status_t status = interface.SetMedia(media);
		if (status != B_OK) {
			fprintf(stderr, "%s: Setting media failed: %s\n", kProgramName,
				strerror(status));
		}
	}

	// start auto configuration, if asked for

	if (doAutoConfig) {
		status_t status = interface.AutoConfigure(family);
		if (status == B_BAD_PORT_ID) {
			fprintf(stderr, "%s: The net_server needs to run for the auto "
				"configuration!\n", kProgramName);
		} else if (status != B_OK) {
			fprintf(stderr, "%s: Auto-configuring failed: %s\n", kProgramName,
				strerror(status));
		}
	}
}


//	#pragma mark -


int
main(int argc, char** argv)
{
	if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")))
		usage(0);

	int socket = ::socket(AF_LINK, SOCK_DGRAM, 0);
	if (socket < 0) {
		fprintf(stderr, "%s: The networking stack doesn't seem to be "
			"available.\n", kProgramName);
		return 1;
	}
	close(socket);

	if (argc > 1
		&& (!strcmp(argv[1], "--delete")
			|| !strcmp(argv[1], "--del")
			|| !strcmp(argv[1], "-d")
			|| !strcmp(argv[1], "del")
			|| !strcmp(argv[1], "delete"))) {
		// Delete interface (addresses)

		if (argc < 3)
			usage(1);

		const char* name = argv[2];
		delete_interface(name, argv + 3, argc - 3);
		return 0;
	}

	if (argc > 1 && !strcmp(argv[1], "-a")) {
		// Accept an optional "-a" option to list all interfaces for those
		// that are used to it from other platforms.

		if (argc > 2)
			usage(1);

		list_interfaces(NULL);
		return 0;
	}

	const char* name = argv[1];
	if (argc > 2) {
		if (configure_wireless(name, argv + 2, argc - 2))
			return 0;

		// Add or configure an interface

		configure_interface(name, argv + 2, argc - 2);
		return 0;
	}

	// list interfaces

	list_interfaces(name);
	return 0;
}
