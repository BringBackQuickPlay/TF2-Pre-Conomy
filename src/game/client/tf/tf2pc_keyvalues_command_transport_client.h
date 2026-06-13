//========= Copyright Valve Corporation, All rights reserved. ============//
//
// TF2PC generic KeyValues command transport - client side
//
// This is a removable workaround for the dedicated-server
// ServerCmdKeyValues() corruption. The original Valve call remains in
// tf_gc_client.cpp and is used whenever this transport is disabled.
//
//=============================================================================

#ifndef TF2PC_KEYVALUES_COMMAND_TRANSPORT_CLIENT_H
#define TF2PC_KEYVALUES_COMMAND_TRANSPORT_CLIENT_H

#include "tf2pc_keyvalues_command_transport_shared.h"

// Runtime opt-in. Both the client and server cvars must be 1.
static ConVar cc_tf2pc_keyvalues_transport(
	"cc_tf2pc_keyvalues_transport",
	"0",
	FCVAR_CLIENTDLL | FCVAR_CHEAT,
	"Use TF2PC's generic serialized-KeyValues command transport instead of Valve's ServerCmdKeyValues()."
);

static ConVar cc_tf2pc_keyvalues_transport_debug(
	"cc_tf2pc_keyvalues_transport_debug",
	"0",
	FCVAR_CLIENTDLL | FCVAR_CHEAT,
	"Print non-sensitive TF2PC KeyValues transport stages to the client console."
);

// Permanent-release alternative:
// Uncomment this definition to force the workaround on in the binary. When
// forced, users cannot disable it through a cvar. Leave it commented while
// testing so Valve's original path remains easy to compare and restore.
// #define TF2PC_FORCE_CLIENT_KEYVALUES_COMMAND_TRANSPORT 1

#ifndef TF2PC_FORCE_CLIENT_KEYVALUES_COMMAND_TRANSPORT
#define TF2PC_FORCE_CLIENT_KEYVALUES_COMMAND_TRANSPORT 0
#endif

#define TF2PC_CLIENT_KV_TRANSPORT_DEBUG( ... ) \
	do \
	{ \
		if ( cc_tf2pc_keyvalues_transport_debug.GetBool() ) \
		{ \
			Msg( __VA_ARGS__ ); \
		} \
	} while ( false )

static bool TF2PCClientKeyValuesTransportEnabled()
{
	return
		TF2PC_FORCE_CLIENT_KEYVALUES_COMMAND_TRANSPORT != 0 ||
		cc_tf2pc_keyvalues_transport.GetBool();
}

//-----------------------------------------------------------------------------
// Send one reliable, text-only command to the game server.
//
// The command buffer is deliberately smaller than CCommand's absolute limit.
// Combined with the 180-byte chunk size, this leaves comfortable room for the
// command name and numeric metadata and prevents silent engine truncation.
//-----------------------------------------------------------------------------
static bool TF2PCSendReliableKeyValuesTransportCommand(
	PRINTF_FORMAT_STRING const char *pszFormat,
	...
)
{
	if ( !engine || !engine->IsConnected() || !pszFormat )
		return false;

	char szCommand[256];

	va_list marker;
	va_start( marker, pszFormat );
	const int nWritten = V_vsnprintf(
		szCommand,
		sizeof( szCommand ),
		pszFormat,
		marker
	);
	va_end( marker );

	if ( nWritten < 0 || nWritten >= (int)sizeof( szCommand ) )
	{
		V_memset( szCommand, 0, sizeof( szCommand ) );
		return false;
	}

	engine->ServerCmd( szCommand, true );

	// A chunk can contain encoded ticket bytes. Do not leave the formatted
	// command in this stack buffer longer than necessary.
	V_memset( szCommand, 0, sizeof( szCommand ) );
	return true;
}

//-----------------------------------------------------------------------------
// Drop-in logical replacement for engine->ServerCmdKeyValues().
//
// Ownership intentionally matches Valve's API: this function always consumes
// pKeyValues, whether sending succeeds or fails.
//-----------------------------------------------------------------------------
static bool TF2PCServerCmdKeyValuesThroughCommandTransport(
	KeyValues *pKeyValues
)
{
	KeyValues::AutoDelete autoDeleteKeyValues( pKeyValues );

	if ( !pKeyValues || !engine || !engine->IsConnected() )
	{
		TF2PC_CLIENT_KV_TRANSPORT_DEBUG(
			"[TF2PC KeyValues transport][CLIENT] Send rejected before serialization: invalid state.\n"
		);
		return false;
	}

	if ( !TF2PCKeyValuesTransportValidateTree( pKeyValues ) )
	{
		TF2PC_CLIENT_KV_TRANSPORT_DEBUG(
			"[TF2PC KeyValues transport][CLIENT] Send rejected: KeyValues tree failed portable-type or complexity validation.\n"
		);
		return false;
	}

	// WriteAsBinary cannot safely encode a TYPE_NONE node with no children.
	// Serialize a temporary copy with private empty-block sentinels, leaving the
	// caller's original tree untouched. The server removes those sentinels.
	KeyValues *pSerializableKeyValues = pKeyValues->MakeCopy();
	KeyValues::AutoDelete autoDeleteSerializable( pSerializableKeyValues );

	if ( !pSerializableKeyValues ||
	     !TF2PCKeyValuesTransportInsertEmptyBlockSentinels(
			pSerializableKeyValues ) ||
	     !TF2PCKeyValuesTransportValidateSerializableTree(
			pSerializableKeyValues ) )
	{
		TF2PC_CLIENT_KV_TRANSPORT_DEBUG(
			"[TF2PC KeyValues transport][CLIENT] Send rejected: failed to prepare a serializable KeyValues copy.\n"
		);
		return false;
	}

	CUtlBuffer binaryBuffer;
	if ( !pSerializableKeyValues->WriteAsBinary( binaryBuffer ) ||
	     !binaryBuffer.IsValid() )
	{
		TF2PC_CLIENT_KV_TRANSPORT_DEBUG(
			"[TF2PC KeyValues transport][CLIENT] Send rejected: WriteAsBinary failed.\n"
		);
		return false;
	}

	const int nBinaryBytes = binaryBuffer.TellPut();
	if ( nBinaryBytes <= 0 ||
	     nBinaryBytes > k_nTF2PCKeyValuesTransportMaxBinaryBytes ||
	     !TF2PCKeyValuesTransportValidateBinary(
			(const unsigned char *)binaryBuffer.Base(),
			nBinaryBytes ) )
	{
		if ( binaryBuffer.Base() && nBinaryBytes > 0 )
			V_memset( binaryBuffer.Base(), 0, nBinaryBytes );

		binaryBuffer.Purge();

		TF2PC_CLIENT_KV_TRANSPORT_DEBUG(
			"[TF2PC KeyValues transport][CLIENT] Send rejected: serialized stream failed validation or exceeded %d bytes.\n",
			k_nTF2PCKeyValuesTransportMaxBinaryBytes
		);
		return false;
	}

	CUtlVector<char> vecEncoded;
	if ( !TF2PCKeyValuesTransportBase64Encode(
			(const unsigned char *)binaryBuffer.Base(),
			nBinaryBytes,
			vecEncoded ) )
	{
		V_memset( binaryBuffer.Base(), 0, nBinaryBytes );
		binaryBuffer.Purge();

		TF2PC_CLIENT_KV_TRANSPORT_DEBUG(
			"[TF2PC KeyValues transport][CLIENT] Send rejected: Base64 encoding failed.\n"
		);
		return false;
	}

	const int nEncodedBytes = vecEncoded.Count() - 1;
	const uint32 unCRC = TF2PCKeyValuesTransportCRC32(
		(const unsigned char *)binaryBuffer.Base(),
		nBinaryBytes
	);

	static uint32 s_unNextTransaction = 0;
	++s_unNextTransaction;
	if ( s_unNextTransaction == 0 )
		++s_unNextTransaction;

	const uint32 unTransaction = s_unNextTransaction;
	const int nChunks =
		( nEncodedBytes + k_nTF2PCKeyValuesTransportChunkBytes - 1 ) /
		k_nTF2PCKeyValuesTransportChunkBytes;

	TF2PC_CLIENT_KV_TRANSPORT_DEBUG(
		"[TF2PC KeyValues transport][CLIENT] Starting transaction=%u binary_bytes=%d encoded_bytes=%d chunks=%d. Payload contents and key names are not logged.\n",
		unTransaction,
		nBinaryBytes,
		nEncodedBytes,
		nChunks
	);

	bool bSucceeded = TF2PCSendReliableKeyValuesTransportCommand(
		"tf2pc_kv_begin %d %u %d %d %08x",
		k_nTF2PCKeyValuesTransportVersion,
		unTransaction,
		nBinaryBytes,
		nEncodedBytes,
		unCRC
	);

	for ( int nOffset = 0;
	      bSucceeded && nOffset < nEncodedBytes;
	      nOffset += k_nTF2PCKeyValuesTransportChunkBytes )
	{
		const int nChunkBytes = Min(
			k_nTF2PCKeyValuesTransportChunkBytes,
			nEncodedBytes - nOffset
		);

		char szChunk[k_nTF2PCKeyValuesTransportChunkBytes + 1];
		V_memcpy( szChunk, vecEncoded.Base() + nOffset, nChunkBytes );
		szChunk[nChunkBytes] = '\0';

		TF2PC_CLIENT_KV_TRANSPORT_DEBUG(
			"[TF2PC KeyValues transport][CLIENT] Sending transaction=%u offset=%d chunk_bytes=%d total=%d/%d.\n",
			unTransaction,
			nOffset,
			nChunkBytes,
			nOffset + nChunkBytes,
			nEncodedBytes
		);

		bSucceeded = TF2PCSendReliableKeyValuesTransportCommand(
			"tf2pc_kv_chunk %u %d %s",
			unTransaction,
			nOffset,
			szChunk
		);

		V_memset( szChunk, 0, sizeof( szChunk ) );
	}

	if ( !bSucceeded )
	{
		// Best-effort cleanup. The server also expires abandoned transactions.
		TF2PCSendReliableKeyValuesTransportCommand(
			"tf2pc_kv_abort %u",
			unTransaction
		);
	}

	V_memset( binaryBuffer.Base(), 0, nBinaryBytes );
	binaryBuffer.Purge();
	TF2PCKeyValuesTransportSecurePurge( vecEncoded );

	TF2PC_CLIENT_KV_TRANSPORT_DEBUG(
		"[TF2PC KeyValues transport][CLIENT] Finished transaction=%u success=%d.\n",
		unTransaction,
		bSucceeded
	);

	return bSucceeded;
}

#endif // TF2PC_KEYVALUES_COMMAND_TRANSPORT_CLIENT_H
