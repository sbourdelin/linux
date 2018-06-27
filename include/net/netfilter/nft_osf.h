#ifndef _NFT_OSF_H
#define _NFT_OSF_H

enum nft_osf_attributes {
	NFTA_OSF_UNSPEC,
	NFTA_OSF_GENRE,
	NFTA_OSF_FLAGS,
	NFTA_OSF_LOGLEVEL,
	NFTA_OSF_TTL,
	__NFTA_OSF_MAX,
};

#define NFTA_OSF_MAX (__NFTA_OSF_MAX - 1)

#endif /* _NFT_OSF_H */
