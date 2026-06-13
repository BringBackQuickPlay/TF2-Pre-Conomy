//========= Copyright Valve Corporation, All rights reserved. ============//
//
// TF2PC generic KeyValues command transport - shared helpers
//
// This file intentionally lives outside Valve's inventory state-machine code.
// Removing the workaround later should only require removing the two transport
// headers and restoring the small call-site/cleanup hooks in tf_gc_client.cpp
// and tf_gc_server.cpp.
//
// The transport serializes a complete KeyValues tree with Valve's existing
// binary KeyValues format. It then uses a URL-safe Base64 wrapper because
// ordinary Source console commands cannot carry arbitrary binary bytes.
//
// IMPORTANT: Base64 is encoding, not encryption. Adding real confidentiality
// here would require an authenticated key exchange or a server public-key
// system. Inventing a home-grown cipher would only create the appearance of
// security. The transport therefore relies on the engine's connection and
// avoids printing, persisting, or unnecessarily copying payload contents.
//
//=============================================================================

#ifndef TF2PC_KEYVALUES_COMMAND_TRANSPORT_SHARED_H
#define TF2PC_KEYVALUES_COMMAND_TRANSPORT_SHARED_H

#include "tier1/KeyValues.h"
#include "tier1/utlbuffer.h"
#include "tier1/utlvector.h"

// Both the client and server expose one three-state cvar:
//   0 = use Valve's original ServerCmdKeyValues path
//   1 = use the TF2PC command transport
//   2 = use the TF2PC command transport and print non-sensitive debug stages
enum ETF2PCKeyValuesTransportMode
{
	k_eTF2PCKeyValuesTransportMode_Valve = 0,
	k_eTF2PCKeyValuesTransportMode_Enabled = 1,
	k_eTF2PCKeyValuesTransportMode_EnabledWithDebug = 2,
};

static const int k_nTF2PCKeyValuesTransportVersion = 1;
static const int k_nTF2PCKeyValuesTransportChunkBytes = 180;
static const int k_nTF2PCKeyValuesTransportMaxBinaryBytes = 16 * 1024;
static const int k_nTF2PCKeyValuesTransportMaxEncodedBytes =
	( ( k_nTF2PCKeyValuesTransportMaxBinaryBytes * 4 ) + 2 ) / 3;
static const int k_nTF2PCKeyValuesTransportMaxNodes = 512;
static const int k_nTF2PCKeyValuesTransportMaxDepth = 32;
static const int k_nTF2PCKeyValuesTransportMaxNameBytes = 255;
static const int k_nTF2PCKeyValuesTransportMaxStringBytes = 4096;

// Valve's binary writer cannot represent a TYPE_NONE node with no children.
// A private sentinel is inserted into a temporary copy before serialization and
// removed after deserialization so genuinely empty blocks survive the trip.
static const char g_szTF2PCKeyValuesTransportEmptyBlockSentinel[] =
	"__tf2pc_empty_block_sentinel__";

//-----------------------------------------------------------------------------
// Utility: bounded C-string length.
//
// Unlike strlen(), this never scans beyond the caller-supplied limit.
//-----------------------------------------------------------------------------
static bool TF2PCKeyValuesTransportGetBoundedStringLength(
	const char *pszValue,
	int nMaximumBytes,
	int &nLength
)
{
	if ( !pszValue || nMaximumBytes < 0 )
		return false;

	for ( int i = 0; i <= nMaximumBytes; ++i )
	{
		if ( pszValue[i] == '\0' )
		{
			nLength = i;
			return true;
		}
	}

	return false;
}

//-----------------------------------------------------------------------------
// Validate a live KeyValues tree before serializing it.
//
// TYPE_PTR is rejected because a process pointer has no meaning on another
// machine and serializing it could disclose an address. TYPE_WSTRING is
// rejected because this SDK branch's binary reader explicitly warns that other
// branches needed security fixes for it. All data used by sdk_inventory fits
// the remaining portable types.
//-----------------------------------------------------------------------------

static bool TF2PCKeyValuesTransportNameIsSafe(
	const char *pszName,
	int nLength
)
{
	if ( !pszName || nLength <= 0 )
		return false;

	for ( int i = 0; i < nLength; ++i )
	{
		const unsigned char c = (unsigned char)pszName[i];

		// Network command names and KeyValues field names are identifiers, not
		// arbitrary binary strings. Reject control characters and non-ASCII
		// bytes so reconstructed names cannot inject misleading console text or
		// exercise unusual symbol-table paths.
		if ( c < 0x20 || c > 0x7E )
			return false;
	}

	return true;
}

static bool TF2PCKeyValuesTransportValidateTreeList(
	KeyValues *pNode,
	int nDepth,
	int &nNodeCount,
	bool bAllowTransportSentinel
)
{
	if ( !pNode || nDepth > k_nTF2PCKeyValuesTransportMaxDepth )
		return false;

	for ( KeyValues *pCurrent = pNode;
	      pCurrent;
	      pCurrent = pCurrent->GetNextKey() )
	{
		if ( ++nNodeCount > k_nTF2PCKeyValuesTransportMaxNodes )
			return false;

		int nNameLength = 0;
		if ( !TF2PCKeyValuesTransportGetBoundedStringLength(
				pCurrent->GetName(),
				k_nTF2PCKeyValuesTransportMaxNameBytes,
				nNameLength ) ||
		     nNameLength <= 0 ||
		     !TF2PCKeyValuesTransportNameIsSafe(
				pCurrent->GetName(),
				nNameLength ) ||
		     ( !bAllowTransportSentinel &&
		       FStrEq(
				pCurrent->GetName(),
				g_szTF2PCKeyValuesTransportEmptyBlockSentinel ) ) )
		{
			return false;
		}

		switch ( pCurrent->GetDataType() )
		{
		case KeyValues::TYPE_NONE:
		{
			KeyValues *pFirstChild = pCurrent->GetFirstSubKey();

			// Empty blocks are valid at the logical API level. The client inserts
			// a private sentinel into a temporary copy before calling Valve's
			// binary writer, which otherwise dereferences a null m_pSub.
			if ( pFirstChild &&
			     !TF2PCKeyValuesTransportValidateTreeList(
					pFirstChild,
					nDepth + 1,
					nNodeCount,
					bAllowTransportSentinel ) )
			{
				return false;
			}
			break;
		}

		case KeyValues::TYPE_STRING:
		{
			int nStringLength = 0;
			if ( !TF2PCKeyValuesTransportGetBoundedStringLength(
					pCurrent->GetString(),
					k_nTF2PCKeyValuesTransportMaxStringBytes,
					nStringLength ) )
			{
				return false;
			}
			break;
		}

		case KeyValues::TYPE_INT:
		case KeyValues::TYPE_FLOAT:
		case KeyValues::TYPE_COLOR:
		case KeyValues::TYPE_UINT64:
			break;

		case KeyValues::TYPE_PTR:
		case KeyValues::TYPE_WSTRING:
		default:
			return false;
		}
	}

	return true;
}

static bool TF2PCKeyValuesTransportValidateTree( KeyValues *pRoot )
{
	if ( !pRoot || pRoot->GetNextKey() )
		return false;

	int nNodeCount = 0;
	return TF2PCKeyValuesTransportValidateTreeList(
		pRoot,
		0,
		nNodeCount,
		false
	);
}

static bool TF2PCKeyValuesTransportValidateSerializableTree(
	KeyValues *pRoot
)
{
	if ( !pRoot || pRoot->GetNextKey() )
		return false;

	int nNodeCount = 0;
	return TF2PCKeyValuesTransportValidateTreeList(
		pRoot,
		0,
		nNodeCount,
		true
	);
}

static bool TF2PCKeyValuesTransportInsertEmptyBlockSentinels(
	KeyValues *pNode
)
{
	for ( KeyValues *pCurrent = pNode;
	      pCurrent;
	      pCurrent = pCurrent->GetNextKey() )
	{
		if ( FStrEq(
				pCurrent->GetName(),
				g_szTF2PCKeyValuesTransportEmptyBlockSentinel ) )
		{
			return false;
		}

		if ( pCurrent->GetDataType() != KeyValues::TYPE_NONE )
			continue;

		KeyValues *pFirstChild = pCurrent->GetFirstSubKey();
		if ( !pFirstChild )
		{
			pCurrent->SetString(
				g_szTF2PCKeyValuesTransportEmptyBlockSentinel,
				"1"
			);
		}
		else if ( !TF2PCKeyValuesTransportInsertEmptyBlockSentinels(
				pFirstChild ) )
		{
			return false;
		}
	}

	return true;
}

static bool TF2PCKeyValuesTransportStripEmptyBlockSentinels(
	KeyValues *pNode
)
{
	for ( KeyValues *pCurrent = pNode;
	      pCurrent;
	      pCurrent = pCurrent->GetNextKey() )
	{
		if ( pCurrent->GetDataType() != KeyValues::TYPE_NONE )
		{
			if ( FStrEq(
					pCurrent->GetName(),
					g_szTF2PCKeyValuesTransportEmptyBlockSentinel ) )
			{
				return false;
			}
			continue;
		}

		KeyValues *pFirstChild = pCurrent->GetFirstSubKey();
		KeyValues *pSentinel = NULL;
		int nChildren = 0;

		for ( KeyValues *pChild = pFirstChild;
		      pChild;
		      pChild = pChild->GetNextKey() )
		{
			++nChildren;
			if ( FStrEq(
					pChild->GetName(),
					g_szTF2PCKeyValuesTransportEmptyBlockSentinel ) )
			{
				pSentinel = pChild;
			}
		}

		if ( pSentinel )
		{
			if ( nChildren != 1 ||
			     pSentinel->GetDataType() != KeyValues::TYPE_STRING ||
			     !FStrEq( pSentinel->GetString(), "1" ) )
			{
				return false;
			}

			pCurrent->RemoveSubKey( pSentinel );
			pSentinel->deleteThis();
		}
		else if ( pFirstChild &&
		          !TF2PCKeyValuesTransportStripEmptyBlockSentinels(
				  pFirstChild ) )
		{
			return false;
		}
	}

	return true;
}

//-----------------------------------------------------------------------------
// CRC32 for accidental corruption detection.
//
// This is not authentication: a malicious sender can calculate another CRC.
// Security decisions still come from the authenticated command client, the
// Steam Web API ticket, and the existing server-side inventory validation.
//-----------------------------------------------------------------------------
static uint32 TF2PCKeyValuesTransportCRC32(
	const unsigned char *pData,
	int nBytes
)
{
	uint32 unCRC = 0xFFFFFFFFu;

	for ( int i = 0; i < nBytes; ++i )
	{
		unCRC ^= pData[i];

		for ( int nBit = 0; nBit < 8; ++nBit )
		{
			const uint32 unMask =
				0u - ( unCRC & 1u );

			unCRC =
				( unCRC >> 1 ) ^
				( 0xEDB88320u & unMask );
		}
	}

	return ~unCRC;
}

//-----------------------------------------------------------------------------
// URL-safe, unpadded Base64.
//
// '-' and '_' replace '+' and '/', so every encoded byte is safe as a single
// console-command argument. Omitting '=' padding saves up to two characters and
// makes the alphabet simpler to validate server-side.
//-----------------------------------------------------------------------------
static const char g_szTF2PCKeyValuesTransportBase64Alphabet[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	"abcdefghijklmnopqrstuvwxyz"
	"0123456789-_";

static int TF2PCKeyValuesTransportBase64Value( char c )
{
	if ( c >= 'A' && c <= 'Z' )
		return c - 'A';

	if ( c >= 'a' && c <= 'z' )
		return 26 + ( c - 'a' );

	if ( c >= '0' && c <= '9' )
		return 52 + ( c - '0' );

	if ( c == '-' )
		return 62;

	if ( c == '_' )
		return 63;

	return -1;
}

static bool TF2PCKeyValuesTransportBase64Encode(
	const unsigned char *pInput,
	int nInputBytes,
	CUtlVector<char> &vecOutput
)
{
	vecOutput.Purge();

	if ( !pInput || nInputBytes <= 0 )
		return false;

	const int nOutputBytes =
		( nInputBytes / 3 ) * 4 +
		( ( nInputBytes % 3 ) == 0 ? 0 : ( nInputBytes % 3 ) + 1 );

	if ( nOutputBytes <= 0 ||
	     nOutputBytes > k_nTF2PCKeyValuesTransportMaxEncodedBytes )
	{
		return false;
	}

	vecOutput.SetCount( nOutputBytes + 1 );
	int nInputOffset = 0;
	int nOutputOffset = 0;

	while ( nInputOffset + 3 <= nInputBytes )
	{
		const uint32 unValue =
			( (uint32)pInput[nInputOffset] << 16 ) |
			( (uint32)pInput[nInputOffset + 1] << 8 ) |
			( (uint32)pInput[nInputOffset + 2] );

		vecOutput[nOutputOffset++] =
			g_szTF2PCKeyValuesTransportBase64Alphabet[( unValue >> 18 ) & 0x3F];
		vecOutput[nOutputOffset++] =
			g_szTF2PCKeyValuesTransportBase64Alphabet[( unValue >> 12 ) & 0x3F];
		vecOutput[nOutputOffset++] =
			g_szTF2PCKeyValuesTransportBase64Alphabet[( unValue >> 6 ) & 0x3F];
		vecOutput[nOutputOffset++] =
			g_szTF2PCKeyValuesTransportBase64Alphabet[unValue & 0x3F];

		nInputOffset += 3;
	}

	const int nRemaining = nInputBytes - nInputOffset;
	if ( nRemaining == 1 )
	{
		const uint32 unValue =
			(uint32)pInput[nInputOffset] << 16;

		vecOutput[nOutputOffset++] =
			g_szTF2PCKeyValuesTransportBase64Alphabet[( unValue >> 18 ) & 0x3F];
		vecOutput[nOutputOffset++] =
			g_szTF2PCKeyValuesTransportBase64Alphabet[( unValue >> 12 ) & 0x3F];
	}
	else if ( nRemaining == 2 )
	{
		const uint32 unValue =
			( (uint32)pInput[nInputOffset] << 16 ) |
			( (uint32)pInput[nInputOffset + 1] << 8 );

		vecOutput[nOutputOffset++] =
			g_szTF2PCKeyValuesTransportBase64Alphabet[( unValue >> 18 ) & 0x3F];
		vecOutput[nOutputOffset++] =
			g_szTF2PCKeyValuesTransportBase64Alphabet[( unValue >> 12 ) & 0x3F];
		vecOutput[nOutputOffset++] =
			g_szTF2PCKeyValuesTransportBase64Alphabet[( unValue >> 6 ) & 0x3F];
	}

	if ( nOutputOffset != nOutputBytes )
	{
		vecOutput.Purge();
		return false;
	}

	vecOutput[nOutputBytes] = '\0';
	return true;
}

static bool TF2PCKeyValuesTransportBase64Decode(
	const char *pInput,
	int nInputBytes,
	CUtlVector<unsigned char> &vecOutput
)
{
	vecOutput.Purge();

	if ( !pInput || nInputBytes <= 0 ||
	     nInputBytes > k_nTF2PCKeyValuesTransportMaxEncodedBytes ||
	     ( nInputBytes % 4 ) == 1 )
	{
		return false;
	}

	const int nRemainder = nInputBytes % 4;
	const int nOutputBytes =
		( nInputBytes / 4 ) * 3 +
		( nRemainder == 2 ? 1 : ( nRemainder == 3 ? 2 : 0 ) );

	if ( nOutputBytes <= 0 ||
	     nOutputBytes > k_nTF2PCKeyValuesTransportMaxBinaryBytes )
	{
		return false;
	}

	vecOutput.SetCount( nOutputBytes );
	int nInputOffset = 0;
	int nOutputOffset = 0;

	while ( nInputOffset + 4 <= nInputBytes )
	{
		const int a = TF2PCKeyValuesTransportBase64Value( pInput[nInputOffset] );
		const int b = TF2PCKeyValuesTransportBase64Value( pInput[nInputOffset + 1] );
		const int c = TF2PCKeyValuesTransportBase64Value( pInput[nInputOffset + 2] );
		const int d = TF2PCKeyValuesTransportBase64Value( pInput[nInputOffset + 3] );

		if ( a < 0 || b < 0 || c < 0 || d < 0 )
		{
			vecOutput.Purge();
			return false;
		}

		const uint32 unValue =
			( (uint32)a << 18 ) |
			( (uint32)b << 12 ) |
			( (uint32)c << 6 ) |
			( (uint32)d );

		vecOutput[nOutputOffset++] = (unsigned char)( unValue >> 16 );
		vecOutput[nOutputOffset++] = (unsigned char)( unValue >> 8 );
		vecOutput[nOutputOffset++] = (unsigned char)unValue;

		nInputOffset += 4;
	}

	if ( nRemainder == 2 )
	{
		const int a = TF2PCKeyValuesTransportBase64Value( pInput[nInputOffset] );
		const int b = TF2PCKeyValuesTransportBase64Value( pInput[nInputOffset + 1] );

		if ( a < 0 || b < 0 )
		{
			vecOutput.Purge();
			return false;
		}

		const uint32 unValue =
			( (uint32)a << 18 ) |
			( (uint32)b << 12 );

		vecOutput[nOutputOffset++] = (unsigned char)( unValue >> 16 );
	}
	else if ( nRemainder == 3 )
	{
		const int a = TF2PCKeyValuesTransportBase64Value( pInput[nInputOffset] );
		const int b = TF2PCKeyValuesTransportBase64Value( pInput[nInputOffset + 1] );
		const int c = TF2PCKeyValuesTransportBase64Value( pInput[nInputOffset + 2] );

		if ( a < 0 || b < 0 || c < 0 )
		{
			vecOutput.Purge();
			return false;
		}

		const uint32 unValue =
			( (uint32)a << 18 ) |
			( (uint32)b << 12 ) |
			( (uint32)c << 6 );

		vecOutput[nOutputOffset++] = (unsigned char)( unValue >> 16 );
		vecOutput[nOutputOffset++] = (unsigned char)( unValue >> 8 );
	}

	if ( nOutputOffset != nOutputBytes )
	{
		vecOutput.Purge();
		return false;
	}

	return true;
}

//-----------------------------------------------------------------------------
// Validate the binary KeyValues stream before passing untrusted bytes into
// KeyValues::ReadAsBinary(). This mirrors the portable subset accepted by the
// live-tree validator and imposes strict limits on names, strings, nodes, and
// recursion depth.
//-----------------------------------------------------------------------------
struct TF2PCKeyValuesTransportBinaryCursor_t
{
	const unsigned char *m_pData;
	int m_nBytes;
	int m_nOffset;
	int m_nNodes;
};

static bool TF2PCKeyValuesTransportBinaryReadCString(
	TF2PCKeyValuesTransportBinaryCursor_t &cursor,
	int nMaximumBytes,
	bool bAllowEmpty,
	bool bValidateAsName
)
{
	const int nStart = cursor.m_nOffset;

	while ( cursor.m_nOffset < cursor.m_nBytes )
	{
		if ( cursor.m_pData[cursor.m_nOffset] == 0 )
		{
			const int nLength = cursor.m_nOffset - nStart;
			++cursor.m_nOffset;

			return
				nLength <= nMaximumBytes &&
				( bAllowEmpty || nLength > 0 ) &&
				( !bValidateAsName ||
				  TF2PCKeyValuesTransportNameIsSafe(
					  (const char *)cursor.m_pData + nStart,
					  nLength ) );
		}

		if ( cursor.m_nOffset - nStart >= nMaximumBytes )
			return false;

		++cursor.m_nOffset;
	}

	return false;
}

static bool TF2PCKeyValuesTransportBinarySkip(
	TF2PCKeyValuesTransportBinaryCursor_t &cursor,
	int nBytes
)
{
	if ( nBytes < 0 ||
	     cursor.m_nOffset > cursor.m_nBytes - nBytes )
	{
		return false;
	}

	cursor.m_nOffset += nBytes;
	return true;
}

static bool TF2PCKeyValuesTransportValidateBinaryList(
	TF2PCKeyValuesTransportBinaryCursor_t &cursor,
	int nDepth,
	int &nListNodes
)
{
	if ( nDepth > k_nTF2PCKeyValuesTransportMaxDepth )
		return false;

	nListNodes = 0;

	while ( true )
	{
		if ( cursor.m_nOffset >= cursor.m_nBytes )
			return false;

		const int nType = cursor.m_pData[cursor.m_nOffset++];

		if ( nType == KeyValues::TYPE_NUMTYPES )
			return true;

		if ( ++cursor.m_nNodes > k_nTF2PCKeyValuesTransportMaxNodes )
			return false;

		++nListNodes;

		if ( !TF2PCKeyValuesTransportBinaryReadCString(
				cursor,
				k_nTF2PCKeyValuesTransportMaxNameBytes,
				false,
				true ) )
		{
			return false;
		}

		switch ( nType )
		{
		case KeyValues::TYPE_NONE:
		{
			int nChildNodes = 0;
			if ( !TF2PCKeyValuesTransportValidateBinaryList(
					cursor,
					nDepth + 1,
					nChildNodes ) ||
			     nChildNodes <= 0 )
			{
				return false;
			}
			break;
		}

		case KeyValues::TYPE_STRING:
			if ( !TF2PCKeyValuesTransportBinaryReadCString(
					cursor,
					k_nTF2PCKeyValuesTransportMaxStringBytes,
					true,
					false ) )
			{
				return false;
			}
			break;

		case KeyValues::TYPE_INT:
		case KeyValues::TYPE_FLOAT:
		case KeyValues::TYPE_COLOR:
			if ( !TF2PCKeyValuesTransportBinarySkip( cursor, 4 ) )
				return false;
			break;

		case KeyValues::TYPE_UINT64:
			if ( !TF2PCKeyValuesTransportBinarySkip( cursor, 8 ) )
				return false;
			break;

		case KeyValues::TYPE_PTR:
		case KeyValues::TYPE_WSTRING:
		default:
			return false;
		}
	}
}

static bool TF2PCKeyValuesTransportValidateBinary(
	const unsigned char *pData,
	int nBytes
)
{
	if ( !pData || nBytes <= 0 ||
	     nBytes > k_nTF2PCKeyValuesTransportMaxBinaryBytes )
	{
		return false;
	}

	TF2PCKeyValuesTransportBinaryCursor_t cursor;
	cursor.m_pData = pData;
	cursor.m_nBytes = nBytes;
	cursor.m_nOffset = 0;
	cursor.m_nNodes = 0;

	int nRootNodes = 0;
	return
		TF2PCKeyValuesTransportValidateBinaryList(
			cursor,
			0,
			nRootNodes ) &&
		nRootNodes == 1 &&
		cursor.m_nOffset == nBytes;
}

//-----------------------------------------------------------------------------
// Securely clear temporary vectors that may contain a Web API ticket or other
// sensitive KeyValues data before releasing their allocations.
//-----------------------------------------------------------------------------
template <typename T>
static void TF2PCKeyValuesTransportSecurePurge( CUtlVector<T> &vecData )
{
	if ( vecData.Count() > 0 && vecData.Base() )
	{
		V_memset(
			vecData.Base(),
			0,
			vecData.Count() * sizeof( T )
		);
	}

	vecData.Purge();
}

#endif // TF2PC_KEYVALUES_COMMAND_TRANSPORT_SHARED_H
