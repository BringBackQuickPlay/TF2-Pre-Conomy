//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_engineer_search_friendly_buildings.cpp
// Engineer moving to plausible observation points to discover friendly Engineer buildings

#include "cbase.h"
#include "nav_mesh.h"
#include "nav_mesh/tf_nav_mesh.h"
#include "tf_player.h"
#include "tf_obj.h"
#include "tf_obj_sentrygun.h"
#include "tf_obj_dispenser.h"
#include "tf_gamerules.h"
#include "trigger_area_capture.h"
#include "bot/tf_bot.h"
#include "bot/behavior/engineer/tf_bot_engineer_search_friendly_buildings.h"
#include "bot/behavior/engineer/tf_bot_engineer_patrol_nest.h"
#include "NextBotUtil.h"


//---------------------------------------------------------------------------------------------
CTFBotEngineerSearchFriendlyBuildings::CTFBotEngineerSearchFriendlyBuildings( void )
{
	m_phase = SEARCH_PHASE_TO_OBSERVATION_POINT;
	m_homeSentry = NULL;
	m_searchGoal = vec3_origin;
	m_failedPathCount = 0;
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotEngineerSearchFriendlyBuildings::OnStart( CTFBot *me, Action< CTFBot > *priorAction )
{
	m_path.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );
	m_path.Invalidate();

	m_repathTimer.Invalidate();
	m_giveUpTimer.Start( 18.0f );
	m_lookTimer.Invalidate();

	m_homeSentry = (CObjectSentrygun *)me->GetObjectOfType( OBJ_SENTRYGUN );
	m_phase = SEARCH_PHASE_TO_OBSERVATION_POINT;
	m_failedPathCount = 0;

	if ( !SelectSearchGoal( me ) )
	{
		if ( !SelectSidestepGoal( me ) )
			return Done( "No friendly-building search goal" );

		m_phase = SEARCH_PHASE_SIDESTEP;
	}

	RecomputeSearchPath( me );
	return Continue();
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerSearchFriendlyBuildings::IsBuildingRecentlyAttacked( CBaseObject *obj, float time ) const
{
	if ( !obj )
		return false;

	if ( obj->HasSapper() || obj->IsPlasmaDisabled() )
		return true;

	if ( obj->GetTimeSinceLastInjury() < time )
		return true;

	if ( obj->GetHealth() < obj->GetMaxHealth() )
		return true;

	return false;
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerSearchFriendlyBuildings::MyImportantBuildingsNeedMe( CTFBot *me ) const
{
	CObjectSentrygun *mySentry = (CObjectSentrygun *)me->GetObjectOfType( OBJ_SENTRYGUN );
	CObjectDispenser *myDispenser = (CObjectDispenser *)me->GetObjectOfType( OBJ_DISPENSER );

	if ( myDispenser && IsBuildingRecentlyAttacked( myDispenser, 1.0f ) )
		return true;

	if ( mySentry && !mySentry->IsMiniBuilding() && IsBuildingRecentlyAttacked( mySentry, 1.0f ) )
		return true;

	if ( mySentry && mySentry->GetTimeSinceLastFired() < 1.0f )
		return true;

	return false;
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerSearchFriendlyBuildings::IsFriendlyBuildingCandidate( CTFBot *me, CBaseObject *obj ) const
{
	if ( !me || !obj )
		return false;

	if ( obj->GetTeamNumber() != me->GetTeamNumber() )
		return false;

	if ( obj->GetBuilder() == me )
		return false;

	if ( FClassnameIs( obj, "obj_sentrygun" ) )
	{
		CObjectSentrygun *sentry = (CObjectSentrygun *)obj;
		if ( sentry->IsMiniBuilding() )
			return false;
	}

	return true;
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerSearchFriendlyBuildings::IsVisibleBuildingForEngineer( CTFBot *me, CBaseObject *obj ) const
{
	if ( !me || !obj )
		return false;

	Vector eye = me->EyePosition();
	Vector center = obj->WorldSpaceCenter();

	Vector toBuilding = center - eye;
	toBuilding.z = 0.0f;

	Vector side( -toBuilding.y, toBuilding.x, 0.0f );
	if ( side.NormalizeInPlace() <= 0.0f )
	{
		side = Vector( 1.0f, 0.0f, 0.0f );
	}

	Vector tracePoints[] =
	{
		center,
		center + Vector( 0.0f, 0.0f, 32.0f ),
		center + side * 24.0f,
		center - side * 24.0f,
		center + side * 24.0f + Vector( 0.0f, 0.0f, 32.0f ),
		center - side * 24.0f + Vector( 0.0f, 0.0f, 32.0f )
	};

	for ( int i = 0; i < ARRAYSIZE( tracePoints ); ++i )
	{
		trace_t result;
		UTIL_TraceLine( eye, tracePoints[i], MASK_SOLID_BRUSHONLY, me, COLLISION_GROUP_NONE, &result );

		if ( result.fraction >= 1.0f )
			return true;

		if ( result.m_pEnt == obj )
			return true;
	}

	return false;
}


//---------------------------------------------------------------------------------------------
CBaseObject *CTFBotEngineerSearchFriendlyBuildings::FindVisibleFriendlyNestTarget( CTFBot *me ) const
{
	CBaseObject *bestSentry = NULL;
	CBaseObject *bestDispenser = NULL;
	float bestSentryRange = FLT_MAX;
	float bestDispenserRange = FLT_MAX;

	CBaseEntity *ent = NULL;
	while ( ( ent = gEntList.FindEntityByClassname( ent, "obj_sentrygun" ) ) != NULL )
	{
		CObjectSentrygun *sentry = dynamic_cast< CObjectSentrygun * >( ent );
		if ( !sentry )
			continue;

		if ( !IsFriendlyBuildingCandidate( me, sentry ) )
			continue;

		if ( !IsVisibleBuildingForEngineer( me, sentry ) )
			continue;

		float range = me->GetDistanceBetween( sentry );
		if ( range < bestSentryRange )
		{
			bestSentry = sentry;
			bestSentryRange = range;
		}
	}

	ent = NULL;
	while ( ( ent = gEntList.FindEntityByClassname( ent, "obj_dispenser" ) ) != NULL )
	{
		CObjectDispenser *dispenser = dynamic_cast< CObjectDispenser * >( ent );
		if ( !dispenser )
			continue;

		if ( !IsFriendlyBuildingCandidate( me, dispenser ) )
			continue;

		if ( !IsVisibleBuildingForEngineer( me, dispenser ) )
			continue;

		float range = me->GetDistanceBetween( dispenser );
		if ( range < bestDispenserRange )
		{
			bestDispenser = dispenser;
			bestDispenserRange = range;
		}
	}

	if ( bestSentry && bestDispenser )
	{
		if ( RandomInt( 1, 100 ) <= 25 )
			return bestSentry;

		return bestDispenser;
	}

	if ( bestSentry )
		return bestSentry;

	return bestDispenser;
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerSearchFriendlyBuildings::SelectSearchGoal( CTFBot *me )
{
	if ( !me || !me->GetLastKnownArea() )
		return false;

	CUtlVector< CNavArea * > nearbyVector;
	CollectSurroundingAreas( &nearbyVector, me->GetLastKnownArea(), 600.0f, me->GetLocomotionInterface()->GetStepHeight(), me->GetLocomotionInterface()->GetStepHeight() );

	if ( nearbyVector.Count() <= 0 )
		return false;

	int myTeam = me->GetTeamNumber();
	int enemyTeam = GetEnemyTeam( myTeam );

	CTeamControlPoint *point = me->GetMyControlPoint();
	CCaptureZone *zone = me->GetFlagCaptureZone();

	bool hasCapturePointGoal = point != NULL;
	bool hasFlagGoal = !hasCapturePointGoal && zone != NULL;
	Vector capturePointGoal = hasCapturePointGoal ? point->GetAbsOrigin() : vec3_origin;
	Vector flagGoal = hasFlagGoal ? zone->WorldSpaceCenter() : vec3_origin;

	CTFNavArea *bestArea = NULL;
	float bestScore = -FLT_MAX;

	CTFNavArea *fallbackArea = NULL;
	float fallbackScore = -FLT_MAX;

	for ( int i = 0; i < nearbyVector.Count(); ++i )
	{
		CTFNavArea *area = (CTFNavArea *)nearbyVector[i];
		if ( !area )
			continue;

		float distanceFromMe = area->GetCenter().DistTo( me->GetAbsOrigin() );
		if ( distanceFromMe < 200.0f )
			continue;

		float myIncursion = area->GetIncursionDistance( myTeam );
		float enemyIncursion = area->GetIncursionDistance( enemyTeam );

		// Respect battle lines when the nav mesh has usable incursion values.
		if ( myIncursion >= 0.0f && enemyIncursion >= 0.0f && myIncursion > enemyIncursion + 300.0f )
			continue;

		float score = RandomFloat( 0.0f, 100.0f );

		// Human-like search order:
		// 1) current control point,
		// 2) intelligence/capture zone,
		// 3) own spawn-side areas.
		if ( hasCapturePointGoal )
		{
			float objectiveRange = area->GetCenter().DistTo( capturePointGoal );
			score += ( objectiveRange < 2200.0f ) ? ( 2200.0f - objectiveRange ) : 0.0f;
		}
		else if ( hasFlagGoal )
		{
			float flagRange = area->GetCenter().DistTo( flagGoal );
			score += ( flagRange < 2200.0f ) ? ( 2200.0f - flagRange ) : 0.0f;
		}
		else
		{
			int spawnRoomFlag = ( myTeam == TF_TEAM_RED ) ? TF_NAV_SPAWN_ROOM_RED : TF_NAV_SPAWN_ROOM_BLUE;
			if ( area->HasAttributeTF( spawnRoomFlag ) )
			{
				score += 1500.0f;
			}

			// If no explicit objective context exists, prefer our side of the map.
			if ( myIncursion >= 0.0f )
			{
				score -= myIncursion * 0.10f;
			}
		}

		// Prefer moderately open observation points, but still keep distance within the 600 HU shell.
		score += area->GetSizeX() * area->GetSizeY() * 0.01f;
		score -= distanceFromMe * 0.10f;

		if ( score > bestScore )
		{
			bestArea = area;
			bestScore = score;
		}

		float simpleFallbackScore = RandomFloat( 0.0f, 100.0f ) + area->GetSizeX() * area->GetSizeY() * 0.01f;
		if ( simpleFallbackScore > fallbackScore )
		{
			fallbackArea = area;
			fallbackScore = simpleFallbackScore;
		}
	}

	CTFNavArea *chosenArea = bestArea ? bestArea : fallbackArea;
	if ( !chosenArea )
		return false;

	m_searchGoal = chosenArea->GetRandomPoint();
	return true;
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerSearchFriendlyBuildings::SelectReturnHomeGoal( CTFBot *me )
{
	CObjectSentrygun *homeSentry = m_homeSentry.Get();
	if ( !homeSentry )
		return false;

	m_searchGoal = homeSentry->GetAbsOrigin();
	return true;
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerSearchFriendlyBuildings::SelectSidestepGoal( CTFBot *me )
{
	if ( !me )
		return false;

	Vector forward;
	me->EyeVectors( &forward );
	forward.z = 0.0f;
	forward.NormalizeInPlace();

	Vector right( -forward.y, forward.x, 0.0f );
	if ( RandomInt( 0, 1 ) == 0 )
	{
		right *= -1.0f;
	}

	m_searchGoal = me->GetAbsOrigin() + right * 66.0f;
	return true;
}


//---------------------------------------------------------------------------------------------
void CTFBotEngineerSearchFriendlyBuildings::RecomputeSearchPath( CTFBot *me )
{
	m_path.Invalidate();

	CTFBotPathCost cost( me, m_phase == SEARCH_PHASE_RETURN_HOME ? FASTEST_ROUTE : SAFEST_ROUTE );
	m_path.Compute( me, m_searchGoal, cost );
	m_repathTimer.Start( RandomFloat( 1.0f, 2.0f ) );
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotEngineerSearchFriendlyBuildings::Update( CTFBot *me, float interval )
{
	if ( MyImportantBuildingsNeedMe( me ) )
		return Done( "My own nest needs me" );

	CBaseObject *target = FindVisibleFriendlyNestTarget( me );
	if ( target )
	{
		return ChangeTo( new CTFBotEngineerPatrolNest( target ), "Found friendly Engineer building while searching" );
	}

	if ( m_giveUpTimer.IsElapsed() && m_phase != SEARCH_PHASE_RETURN_HOME )
	{
		m_phase = SEARCH_PHASE_RETURN_HOME;
		if ( !SelectReturnHomeGoal( me ) )
			return Done( "Search timed out and no home sentry exists" );

		RecomputeSearchPath( me );
	}

	const float goalRange = 50.0f;
	if ( me->GetAbsOrigin().DistToSqr( m_searchGoal ) <= goalRange * goalRange )
	{
		if ( m_phase == SEARCH_PHASE_TO_OBSERVATION_POINT )
		{
			m_phase = SEARCH_PHASE_LOOK_AROUND;
			m_lookTimer.Start( RandomFloat( 1.5f, 2.5f ) );
			return Continue();
		}

		if ( m_phase == SEARCH_PHASE_LOOK_AROUND )
		{
			// fall through below until look timer elapses
		}
		else if ( m_phase == SEARCH_PHASE_SIDESTEP )
		{
			m_phase = SEARCH_PHASE_RETURN_HOME;
			if ( !SelectReturnHomeGoal( me ) )
				return Done( "Finished search sidestep" );

			RecomputeSearchPath( me );
			return Continue();
		}
		else if ( m_phase == SEARCH_PHASE_RETURN_HOME )
		{
			return Done( "Returned home after friendly building search" );
		}
	}

	if ( m_phase == SEARCH_PHASE_LOOK_AROUND )
	{
		me->StartLookingAroundForEnemies();

		if ( !m_lookTimer.IsElapsed() )
			return Continue();

		m_phase = SEARCH_PHASE_SIDESTEP;
		if ( !SelectSidestepGoal( me ) )
		{
			m_phase = SEARCH_PHASE_RETURN_HOME;
			if ( !SelectReturnHomeGoal( me ) )
				return Done( "Finished looking around while searching" );
		}

		RecomputeSearchPath( me );
		return Continue();
	}

	if ( m_repathTimer.IsElapsed() || !m_path.IsValid() )
	{
		RecomputeSearchPath( me );
	}

	CBaseCombatWeapon *shotgun = me->Weapon_GetSlot( TF_WPN_TYPE_PRIMARY );
	if ( shotgun )
	{
		me->Weapon_Switch( shotgun );
	}

	me->StartLookingAroundForEnemies();
	m_path.Update( me );

	return Continue();
}


//---------------------------------------------------------------------------------------------
void CTFBotEngineerSearchFriendlyBuildings::OnEnd( CTFBot *me, Action< CTFBot > *nextAction )
{
	me->StartLookingAroundForEnemies();
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotEngineerSearchFriendlyBuildings::OnResume( CTFBot *me, Action< CTFBot > *interruptingAction )
{
	m_path.Invalidate();
	m_repathTimer.Invalidate();
	m_giveUpTimer.Reset();

	return Continue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotEngineerSearchFriendlyBuildings::OnStuck( CTFBot *me )
{
	++m_failedPathCount;

	if ( m_failedPathCount >= 2 )
	{
		m_phase = SEARCH_PHASE_RETURN_HOME;
		if ( !SelectReturnHomeGoal( me ) )
			return TryDone( RESULT_TRY, "Search stuck and no home sentry exists" );
	}
	else if ( m_phase != SEARCH_PHASE_RETURN_HOME )
	{
		SelectSearchGoal( me );
	}

	RecomputeSearchPath( me );
	return TryContinue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotEngineerSearchFriendlyBuildings::OnMoveToSuccess( CTFBot *me, const Path *path )
{
	return TryContinue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotEngineerSearchFriendlyBuildings::OnMoveToFailure( CTFBot *me, const Path *path, MoveToFailureType reason )
{
	++m_failedPathCount;

	if ( m_failedPathCount >= 2 )
	{
		m_phase = SEARCH_PHASE_RETURN_HOME;
		if ( !SelectReturnHomeGoal( me ) )
			return TryDone( RESULT_TRY, "Search path failed and no home sentry exists" );
	}
	else if ( m_phase != SEARCH_PHASE_RETURN_HOME )
	{
		SelectSearchGoal( me );
	}

	RecomputeSearchPath( me );
	return TryContinue();
}


//---------------------------------------------------------------------------------------------
QueryResultType CTFBotEngineerSearchFriendlyBuildings::ShouldHurry( const INextBot *me ) const
{
	return ANSWER_YES;
}
