//========= Copyright Valve Corporation, All rights reserved. ============//
//
// TF2PC generic KeyValues command transport - server side
//
// This file is deliberately isolated from Valve's inventory code. It accepts a
// strictly bounded serialized KeyValues tree, reconstructs it inside the server
// game DLL (thereby creating valid local KeyValues symbols), and dispatches it
// through the same CTFGameRules::ClientCommandKeyValues() handler used by the
// engine's original ServerCmdKeyValues path.
//
//=============================================================================

#ifndef TF2PC_KEYVALUES_COMMAND_TRANSPORT_SERVER_H
#define TF2PC_KEYVALUES_COMMAND_TRANSPORT_SERVER_H

#include "tf2pc_keyvalues_command_transport_shared.h"

// One runtime switch controls both transport selection and debug output:
//   0 = accept only Valve's original ServerCmdKeyValues path
//   1 = accept TF2PC's generic serialized-KeyValues command transport
//   2 = accept TF2PC's transport plus non-sensitive server debugging
static ConVar sv_tf2pc_keyvalues_transport(
	"sv_tf2pc_keyvalues_transport",
	"0",
	FCVAR_GAMEDLL,
	"KeyValues transport mode: 0=Valve, 1=TF2PC transport, 2=TF2PC transport with debug output.",
	true,
	(float)k_eTF2PCKeyValuesTransportMode_Valve,
	true,
	(float)k_eTF2PCKeyValuesTransportMode_EnabledWithDebug
);

// Permanent-release alternative:
// Uncomment this definition to force mode 1 in the binary. Server owners then
// cannot disable the replacement through the cvar. Use 2 instead only for a
// permanently verbose diagnostic build.
// #define TF2PC_FORCE_SERVER_KEYVALUES_COMMAND_TRANSPORT_MODE 1

#ifndef TF2PC_FORCE_SERVER_KEYVALUES_COMMAND_TRANSPORT_MODE
#define TF2PC_FORCE_SERVER_KEYVALUES_COMMAND_TRANSPORT_MODE 0
#endif

static int TF2PCServerKeyValuesTransportMode()
{
	if ( TF2PC_FORCE_SERVER_KEYVALUES_COMMAND_TRANSPORT_MODE != 0 )
		return TF2PC_FORCE_SERVER_KEYVALUES_COMMAND_TRANSPORT_MODE;

	return sv_tf2pc_keyvalues_transport.GetInt();
}

static bool TF2PCServerKeyValuesTransportEnabled()
{
	return
		TF2PCServerKeyValuesTransportMode() >=
		k_eTF2PCKeyValuesTransportMode_Enabled;
}

static bool TF2PCServerKeyValuesTransportDebugEnabled()
{
	return
		TF2PCServerKeyValuesTransportMode() >=
		k_eTF2PCKeyValuesTransportMode_EnabledWithDebug;
}

#define TF2PC_SERVER_KV_TRANSPORT_DEBUG( ... ) \
	do \
	{ \
		if ( TF2PCServerKeyValuesTransportDebugEnabled() ) \
		{ \
			Msg( __VA_ARGS__ ); \
		} \
	} while ( false )

static const double k_flTF2PCKeyValuesTransportTimeout = 15.0;
static const double k_flTF2PCKeyValuesTransportMinimumBeginInterval = 0.25;

struct TF2PCKeyValuesTransportState_t
{
	TF2PCKeyValuesTransportState_t()
		: m_flLastBeginTime( -1000.0 )
		, m_flLastRejectLogTime( -1000.0 )
	{
		Reset();
	}

	void Reset()
	{
		TF2PCKeyValuesTransportSecurePurge( m_vecEncoded );

		m_bActive = false;
		m_unTransaction = 0;
		m_steamID.Clear();
		m_nExpectedBinaryBytes = 0;
		m_nExpectedEncodedBytes = 0;
		m_nReceivedEncodedBytes = 0;
		m_unExpectedCRC = 0;
		m_flStartedAt = 0.0;
	}

	bool IsExpired() const
	{
		return
			m_bActive &&
			Plat_FloatTime() - m_flStartedAt >
				k_flTF2PCKeyValuesTransportTimeout;
	}

	bool m_bActive;
	uint32 m_unTransaction;
	CSteamID m_steamID;
	int m_nExpectedBinaryBytes;
	int m_nExpectedEncodedBytes;
	int m_nReceivedEncodedBytes;
	uint32 m_unExpectedCRC;
	CUtlVector<char> m_vecEncoded;
	double m_flStartedAt;
	double m_flLastBeginTime;
	double m_flLastRejectLogTime;
};

static TF2PCKeyValuesTransportState_t
	g_TF2PCKeyValuesTransportStates[MAX_PLAYERS + 1];

//-----------------------------------------------------------------------------
// Strict numeric parsers. atoi()/strtoul()-style partial parsing is avoided so
// malformed command arguments cannot silently become valid metadata.
//-----------------------------------------------------------------------------
static bool TF2PCKeyValuesTransportParseUInt32(
	const char *pszValue,
	uint32 &unResult
)
{
	if ( !pszValue || !pszValue[0] )
		return false;

	uint64 unValue = 0;

	for ( const char *p = pszValue; *p; ++p )
	{
		if ( *p < '0' || *p > '9' )
			return false;

		const uint64 unDigit = (uint64)( *p - '0' );
		if ( unValue > ( 0xFFFFFFFFULL - unDigit ) / 10ULL )
			return false;

		unValue = unValue * 10ULL + unDigit;
	}

	unResult = (uint32)unValue;
	return true;
}

static bool TF2PCKeyValuesTransportParsePositiveInt(
	const char *pszValue,
	int nMaximum,
	int &nResult
)
{
	uint32 unValue = 0;
	if ( !TF2PCKeyValuesTransportParseUInt32( pszValue, unValue ) ||
	     unValue > (uint32)nMaximum )
	{
		return false;
	}

	nResult = (int)unValue;
	return true;
}

static bool TF2PCKeyValuesTransportParseHex32(
	const char *pszValue,
	uint32 &unResult
)
{
	if ( !pszValue || !pszValue[0] )
		return false;

	uint32 unValue = 0;
	int nDigits = 0;

	for ( const char *p = pszValue; *p; ++p )
	{
		if ( ++nDigits > 8 )
			return false;

		uint32 unDigit = 0;
		if ( *p >= '0' && *p <= '9' )
			unDigit = (uint32)( *p - '0' );
		else if ( *p >= 'a' && *p <= 'f' )
			unDigit = 10u + (uint32)( *p - 'a' );
		else if ( *p >= 'A' && *p <= 'F' )
			unDigit = 10u + (uint32)( *p - 'A' );
		else
			return false;

		unValue = ( unValue << 4 ) | unDigit;
	}

	unResult = unValue;
	return true;
}

//-----------------------------------------------------------------------------
// Obtain and authenticate the player associated with this command.
//
// No SteamID, player index, or ownership claim is accepted from payload data.
// The transport is always tied to the actual command client's connection.
//-----------------------------------------------------------------------------
static bool TF2PCKeyValuesTransportGetCommandContext(
	CTFPlayer *&pTFPlayer,
	CSteamID &steamID,
	TF2PCKeyValuesTransportState_t *&pState
)
{
	pTFPlayer = NULL;
	pState = NULL;

	if ( !TF2PCServerKeyValuesTransportEnabled() )
		return false;

	pTFPlayer = ToTFPlayer( UTIL_GetCommandClient() );
	if ( !pTFPlayer || pTFPlayer->IsFakeClient() )
		return false;

	const int nPlayerIndex = pTFPlayer->entindex();
	if ( nPlayerIndex <= 0 || nPlayerIndex > MAX_PLAYERS )
		return false;

	if ( !pTFPlayer->GetSteamID( &steamID ) ||
	     !steamID.IsValid() ||
	     !steamID.BIndividualAccount() )
	{
		return false;
	}

	pState = &g_TF2PCKeyValuesTransportStates[nPlayerIndex];

	if ( pState->IsExpired() )
	{
		TF2PC_SERVER_KV_TRANSPORT_DEBUG(
			"[TF2PC KeyValues transport][SERVER] Expired incomplete transaction=%u for entity_index=%d.\n",
			pState->m_unTransaction,
			nPlayerIndex
		);

		pState->Reset();
	}

	return true;
}

static void TF2PCKeyValuesTransportReject(
	CTFPlayer *pTFPlayer,
	TF2PCKeyValuesTransportState_t *pState,
	const char *pszStage
)
{
	if ( pState && TF2PCServerKeyValuesTransportDebugEnabled() )
	{
		const double flNow = Plat_FloatTime();

		// A malicious client must not be able to turn debug mode into unlimited
		// console spam. Log at most one rejection per player per second.
		if ( flNow - pState->m_flLastRejectLogTime >= 1.0 )
		{
			pState->m_flLastRejectLogTime = flNow;

			Msg(
				"[TF2PC KeyValues transport][SERVER] Rejected at stage=\"%s\" entity_index=%d active=%d transaction=%u received=%d/%d. Payload contents are not logged.\n",
				pszStage ? pszStage : "unknown",
				pTFPlayer ? pTFPlayer->entindex() : -1,
				pState->m_bActive,
				pState->m_unTransaction,
				pState->m_nReceivedEncodedBytes,
				pState->m_nExpectedEncodedBytes
			);
		}
	}

	if ( pState )
		pState->Reset();
}

static bool TF2PCKeyValuesTransportMatchesTransaction(
	const TF2PCKeyValuesTransportState_t &state,
	const CSteamID &steamID,
	uint32 unTransaction
)
{
	return
		state.m_bActive &&
		state.m_unTransaction == unTransaction &&
		state.m_steamID == steamID;
}

//-----------------------------------------------------------------------------
// Finish a fully received transaction.
//-----------------------------------------------------------------------------
static void TF2PCKeyValuesTransportFinalize(
	CTFPlayer *pTFPlayer,
	TF2PCKeyValuesTransportState_t *pState
)
{
	if ( !pTFPlayer || !pState || !pState->m_bActive )
		return;

	const uint32 unTransaction = pState->m_unTransaction;

	CUtlVector<unsigned char> vecBinary;
	if ( !TF2PCKeyValuesTransportBase64Decode(
			pState->m_vecEncoded.Base(),
			pState->m_nExpectedEncodedBytes,
			vecBinary ) ||
	     vecBinary.Count() != pState->m_nExpectedBinaryBytes )
	{
		TF2PCKeyValuesTransportSecurePurge( vecBinary );
		TF2PCKeyValuesTransportReject(
			pTFPlayer,
			pState,
			"Base64 decode or decoded-length validation"
		);
		return;
	}

	if ( TF2PCKeyValuesTransportCRC32(
			vecBinary.Base(),
			vecBinary.Count() ) != pState->m_unExpectedCRC )
	{
		TF2PCKeyValuesTransportSecurePurge( vecBinary );
		TF2PCKeyValuesTransportReject(
			pTFPlayer,
			pState,
			"CRC validation"
		);
		return;
	}

	if ( !TF2PCKeyValuesTransportValidateBinary(
			vecBinary.Base(),
			vecBinary.Count() ) )
	{
		TF2PCKeyValuesTransportSecurePurge( vecBinary );
		TF2PCKeyValuesTransportReject(
			pTFPlayer,
			pState,
			"binary KeyValues pre-validation"
		);
		return;
	}

	CUtlBuffer binaryBuffer(
		vecBinary.Base(),
		vecBinary.Count(),
		CUtlBuffer::READ_ONLY
	);

	KeyValues *pKeyValues = new KeyValues( "" );
	const bool bReadSucceeded =
		pKeyValues->ReadAsBinary( binaryBuffer ) &&
		binaryBuffer.IsValid() &&
		binaryBuffer.GetBytesRemaining() == 0 &&
		TF2PCKeyValuesTransportValidateSerializableTree( pKeyValues ) &&
		TF2PCKeyValuesTransportStripEmptyBlockSentinels( pKeyValues ) &&
		TF2PCKeyValuesTransportValidateTree( pKeyValues );

	if ( !bReadSucceeded )
	{
		pKeyValues->deleteThis();
		TF2PCKeyValuesTransportSecurePurge( vecBinary );
		TF2PCKeyValuesTransportReject(
			pTFPlayer,
			pState,
			"ReadAsBinary or reconstructed-tree validation"
		);
		return;
	}

	TF2PC_SERVER_KV_TRANSPORT_DEBUG(
		"[TF2PC KeyValues transport][SERVER] Reconstructed transaction=%u entity_index=%d binary_bytes=%d. Dispatching through ClientCommandKeyValues; payload contents and key names are not logged.\n",
		unTransaction,
		pTFPlayer->entindex(),
		vecBinary.Count()
	);

	// Dispatch through Valve's existing game-rule handler. This is the key
	// property that keeps the transport generic: it does not know or care
	// whether the root is sdk_inventory or another KeyValues command.
	if ( !TFGameRules() )
	{
		pKeyValues->deleteThis();
		TF2PCKeyValuesTransportSecurePurge( vecBinary );
		TF2PCKeyValuesTransportReject(
			pTFPlayer,
			pState,
			"game-rules dispatch availability"
		);
		return;
	}

	TFGameRules()->ClientCommandKeyValues(
		pTFPlayer->edict(),
		pKeyValues
	);

	pKeyValues->deleteThis();
	TF2PCKeyValuesTransportSecurePurge( vecBinary );
	pState->Reset();

	TF2PC_SERVER_KV_TRANSPORT_DEBUG(
		"[TF2PC KeyValues transport][SERVER] Completed transaction=%u entity_index=%d.\n",
		unTransaction,
		pTFPlayer->entindex()
	);
}

//-----------------------------------------------------------------------------
// Commands
//-----------------------------------------------------------------------------
static void CC_TF2PCKeyValuesTransportBegin( const CCommand &args )
{
	CTFPlayer *pTFPlayer = NULL;
	CSteamID steamID;
	TF2PCKeyValuesTransportState_t *pState = NULL;

	if ( !TF2PCKeyValuesTransportGetCommandContext(
			pTFPlayer,
			steamID,
			pState ) )
	{
		return;
	}

	if ( args.ArgC() != 6 )
	{
		TF2PCKeyValuesTransportReject(
			pTFPlayer,
			pState,
			"begin argument count"
		);
		return;
	}

	int nVersion = 0;
	uint32 unTransaction = 0;
	int nBinaryBytes = 0;
	int nEncodedBytes = 0;
	uint32 unCRC = 0;

	if ( !TF2PCKeyValuesTransportParsePositiveInt(
			args[1],
			INT_MAX,
			nVersion ) ||
	     !TF2PCKeyValuesTransportParseUInt32(
			args[2],
			unTransaction ) ||
	     !TF2PCKeyValuesTransportParsePositiveInt(
			args[3],
			k_nTF2PCKeyValuesTransportMaxBinaryBytes,
			nBinaryBytes ) ||
	     !TF2PCKeyValuesTransportParsePositiveInt(
			args[4],
			k_nTF2PCKeyValuesTransportMaxEncodedBytes,
			nEncodedBytes ) ||
	     !TF2PCKeyValuesTransportParseHex32(
			args[5],
			unCRC ) ||
	     nVersion != k_nTF2PCKeyValuesTransportVersion ||
	     unTransaction == 0 ||
	     nBinaryBytes <= 0 ||
	     nEncodedBytes !=
			( nBinaryBytes / 3 ) * 4 +
			( ( nBinaryBytes % 3 ) == 0 ? 0 : ( nBinaryBytes % 3 ) + 1 ) )
	{
		TF2PCKeyValuesTransportReject(
			pTFPlayer,
			pState,
			"begin metadata validation"
		);
		return;
	}

	const double flNow = Plat_FloatTime();
	if ( flNow - pState->m_flLastBeginTime <
	     k_flTF2PCKeyValuesTransportMinimumBeginInterval )
	{
		TF2PCKeyValuesTransportReject(
			pTFPlayer,
			pState,
			"begin rate limit"
		);
		return;
	}

	pState->Reset();
	pState->m_flLastBeginTime = flNow;
	pState->m_bActive = true;
	pState->m_unTransaction = unTransaction;
	pState->m_steamID = steamID;
	pState->m_nExpectedBinaryBytes = nBinaryBytes;
	pState->m_nExpectedEncodedBytes = nEncodedBytes;
	pState->m_nReceivedEncodedBytes = 0;
	pState->m_unExpectedCRC = unCRC;
	pState->m_flStartedAt = flNow;
	pState->m_vecEncoded.SetCount( nEncodedBytes + 1 );
	V_memset(
		pState->m_vecEncoded.Base(),
		0,
		pState->m_vecEncoded.Count()
	);

	TF2PC_SERVER_KV_TRANSPORT_DEBUG(
		"[TF2PC KeyValues transport][SERVER] Begin accepted: entity_index=%d transaction=%u binary_bytes=%d encoded_bytes=%d expected_chunks=%d. Payload contents are not logged.\n",
		pTFPlayer->entindex(),
		unTransaction,
		nBinaryBytes,
		nEncodedBytes,
		( nEncodedBytes + k_nTF2PCKeyValuesTransportChunkBytes - 1 ) /
			k_nTF2PCKeyValuesTransportChunkBytes
	);
}

static void CC_TF2PCKeyValuesTransportChunk( const CCommand &args )
{
	CTFPlayer *pTFPlayer = NULL;
	CSteamID steamID;
	TF2PCKeyValuesTransportState_t *pState = NULL;

	if ( !TF2PCKeyValuesTransportGetCommandContext(
			pTFPlayer,
			steamID,
			pState ) )
	{
		return;
	}

	if ( args.ArgC() != 4 )
	{
		// Do not log inactive garbage commands. They are common attack/spam
		// material and contain no useful debugging state.
		if ( pState->m_bActive )
		{
			TF2PCKeyValuesTransportReject(
				pTFPlayer,
				pState,
				"chunk argument count"
			);
		}
		return;
	}

	uint32 unTransaction = 0;
	int nOffset = 0;

	if ( !TF2PCKeyValuesTransportParseUInt32(
			args[1],
			unTransaction ) ||
	     !TF2PCKeyValuesTransportParsePositiveInt(
			args[2],
			k_nTF2PCKeyValuesTransportMaxEncodedBytes,
			nOffset ) ||
	     !TF2PCKeyValuesTransportMatchesTransaction(
			*pState,
			steamID,
			unTransaction ) )
	{
		if ( pState->m_bActive )
		{
			TF2PCKeyValuesTransportReject(
				pTFPlayer,
				pState,
				"chunk transaction or offset metadata"
			);
		}
		return;
	}

	const char *pszChunk = args[3];
	int nChunkBytes = 0;

	if ( !TF2PCKeyValuesTransportGetBoundedStringLength(
			pszChunk,
			k_nTF2PCKeyValuesTransportChunkBytes,
			nChunkBytes ) ||
	     nChunkBytes <= 0 )
	{
		TF2PCKeyValuesTransportReject(
			pTFPlayer,
			pState,
			"chunk length"
		);
		return;
	}

	for ( int i = 0; i < nChunkBytes; ++i )
	{
		if ( TF2PCKeyValuesTransportBase64Value( pszChunk[i] ) < 0 )
		{
			TF2PCKeyValuesTransportReject(
				pTFPlayer,
				pState,
				"chunk alphabet"
			);
			return;
		}
	}

	if ( nOffset != pState->m_nReceivedEncodedBytes ||
	     nOffset > pState->m_nExpectedEncodedBytes - nChunkBytes )
	{
		TF2PCKeyValuesTransportReject(
			pTFPlayer,
			pState,
			"chunk ordering or declared-length bounds"
		);
		return;
	}

	V_memcpy(
		pState->m_vecEncoded.Base() + nOffset,
		pszChunk,
		nChunkBytes
	);

	pState->m_nReceivedEncodedBytes += nChunkBytes;
	pState->m_vecEncoded[pState->m_nExpectedEncodedBytes] = '\0';

	TF2PC_SERVER_KV_TRANSPORT_DEBUG(
		"[TF2PC KeyValues transport][SERVER] Chunk accepted: entity_index=%d transaction=%u offset=%d chunk_bytes=%d total=%d/%d.\n",
		pTFPlayer->entindex(),
		unTransaction,
		nOffset,
		nChunkBytes,
		pState->m_nReceivedEncodedBytes,
		pState->m_nExpectedEncodedBytes
	);

	// No separate commit command is needed. Exact sequential lengths make the
	// final chunk an unambiguous commit point.
	if ( pState->m_nReceivedEncodedBytes ==
	     pState->m_nExpectedEncodedBytes )
	{
		TF2PCKeyValuesTransportFinalize(
			pTFPlayer,
			pState
		);
	}
}

static void CC_TF2PCKeyValuesTransportAbort( const CCommand &args )
{
	CTFPlayer *pTFPlayer = NULL;
	CSteamID steamID;
	TF2PCKeyValuesTransportState_t *pState = NULL;

	if ( !TF2PCKeyValuesTransportGetCommandContext(
			pTFPlayer,
			steamID,
			pState ) ||
	     args.ArgC() != 2 )
	{
		return;
	}

	uint32 unTransaction = 0;
	if ( TF2PCKeyValuesTransportParseUInt32(
			args[1],
			unTransaction ) &&
	     TF2PCKeyValuesTransportMatchesTransaction(
			*pState,
			steamID,
			unTransaction ) )
	{
		TF2PC_SERVER_KV_TRANSPORT_DEBUG(
			"[TF2PC KeyValues transport][SERVER] Aborted transaction=%u entity_index=%d.\n",
			unTransaction,
			pTFPlayer->entindex()
		);

		pState->Reset();
	}
}

static ConCommand tf2pc_kv_begin(
	"tf2pc_kv_begin",
	CC_TF2PCKeyValuesTransportBegin,
	"Internal TF2PC generic KeyValues transport begin command.",
	FCVAR_GAMEDLL | FCVAR_HIDDEN
);

static ConCommand tf2pc_kv_chunk(
	"tf2pc_kv_chunk",
	CC_TF2PCKeyValuesTransportChunk,
	"Internal TF2PC generic KeyValues transport chunk command.",
	FCVAR_GAMEDLL | FCVAR_HIDDEN
);

static ConCommand tf2pc_kv_abort(
	"tf2pc_kv_abort",
	CC_TF2PCKeyValuesTransportAbort,
	"Internal TF2PC generic KeyValues transport abort command.",
	FCVAR_GAMEDLL | FCVAR_HIDDEN
);

//-----------------------------------------------------------------------------
// Lifecycle cleanup hooks called by tf_gc_server.cpp.
//-----------------------------------------------------------------------------
static void TF2PCClearKeyValuesTransportForSteamID(
	const CSteamID &steamID
)
{
	if ( !steamID.IsValid() )
		return;

	for ( int i = 1; i <= MAX_PLAYERS; ++i )
	{
		TF2PCKeyValuesTransportState_t &state =
			g_TF2PCKeyValuesTransportStates[i];

		if ( state.m_bActive && state.m_steamID == steamID )
			state.Reset();
	}
}

static void TF2PCClearAllKeyValuesTransportStates()
{
	for ( int i = 1; i <= MAX_PLAYERS; ++i )
		g_TF2PCKeyValuesTransportStates[i].Reset();
}

#endif // TF2PC_KEYVALUES_COMMAND_TRANSPORT_SERVER_H
