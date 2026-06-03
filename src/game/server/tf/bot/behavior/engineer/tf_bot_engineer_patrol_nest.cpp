//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_engineer_patrol_nest.cpp
// Engineer patrolling/searching between his nest and nearby friendly Engineer buildings

#include "cbase.h"
#include "nav_mesh.h"
#include "nav_mesh/tf_nav_mesh.h"
#include "tf_player.h"
#include "tf_obj.h"
#include "tf_obj_sentrygun.h"
#include "tf_obj_dispenser.h"
#include "tf_gamerules.h"
#include "bot/tf_bot.h"
#include "bot/behavior/engineer/tf_bot_engineer_patrol_nest.h"
#include "bot/behavior/engineer/tf_bot_engineer_assist_friendly_building.h"
#include "NextBotUtil.h"


//---------------------------------------------------------------------------------------------
CTFBotEngineerPatrolNest::CTFBotEngineerPatrolNest( void )
{
	m_mode = PATROL_MODE_SEARCH_FOR_FRIENDLY_BUILDINGS;
	m_searchPhase = SEARCH_PHASE_SIDESTEP;
	m_targetBuilding = NULL;
	m_searchGoal = vec3_origin;
	m_hasSearchGoal = false;
	m_hasArrived = false;
	m_shouldSpyCheck = false;
	m_spyCheckShotsRemaining = 0;
}


//---------------------------------------------------------------------------------------------
CTFBotEngineerPatrolNest::CTFBotEngineerPatrolNest( CBaseObject *targetBuilding )
{
	m_mode = PATROL_MODE_TARGET;
	m_searchPhase = SEARCH_PHASE_SIDESTEP;
	m_targetBuilding = targetBuilding;
	m_searchGoal = vec3_origin;
	m_hasSearchGoal = false;
	m_hasArrived = false;
	m_shouldSpyCheck = false;
	m_spyCheckShotsRemaining = 0;
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotEngineerPatrolNest::OnStart( CTFBot *me, Action< CTFBot > *priorAction )
{
	m_path.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );
	m_path.Invalidate();

	m_repathTimer.Invalidate();
	m_waitTimer.Invalidate();
	m_spyCheckShotTimer.Invalidate();

	m_hasArrived = false;
	m_shouldSpyCheck = RandomInt( 1, 100 ) <= 25;
	m_spyCheckShotsRemaining = m_shouldSpyCheck ? RandomInt( 1, 2 ) : 0;

	if ( m_mode == PATROL_MODE_SEARCH_FOR_FRIENDLY_BUILDINGS )
	{
		// Search mode should visibly move to a nearby safe observation point first.
		// Sidestepping is only a fallback/local adjustment, not the whole search.
		m_searchPhase = SEARCH_PHASE_WANDER;
		m_hasSearchGoal = SelectSearchGoal( me );
		if ( !m_hasSearchGoal )
		{
			m_searchPhase = SEARCH_PHASE_SIDESTEP;
			m_hasSearchGoal = SelectSearchGoal( me );
		}
		m_giveUpTimer.Start( 18.0f );
	}
	else
	{
		m_giveUpTimer.Start( 15.0f );
	}

	return Continue();
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerPatrolNest::IsTargetValid( CTFBot *me, CBaseObject *target ) const
{
	if ( !me || !target )
		return false;

	if ( target->GetTeamNumber() != me->GetTeamNumber() )
		return false;

	if ( target->GetBuilder() == me )
		return false;

	return true;
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerPatrolNest::IsBuildingRecentlyAttacked( CBaseObject *obj, float time ) const
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
bool CTFBotEngineerPatrolNest::MyImportantBuildingsNeedMe( CTFBot *me ) const
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
bool CTFBotEngineerPatrolNest::IsVisibleBuildingForEngineer( CTFBot *me, CBaseObject *obj ) const
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
CBaseObject *CTFBotEngineerPatrolNest::FindVisibleFriendlyNestTarget( CTFBot *me ) const
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

		if ( !IsTargetValid( me, sentry ) )
			continue;

		if ( sentry->IsMiniBuilding() )
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

		if ( !IsTargetValid( me, dispenser ) )
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
bool CTFBotEngineerPatrolNest::SelectSearchGoal( CTFBot *me )
{
	if ( !me )
		return false;

	if ( m_searchPhase == SEARCH_PHASE_SIDESTEP )
	{
		Vector forward;
		me->EyeVectors( &forward );
		forward.z = 0.0f;
		if ( forward.NormalizeInPlace() <= 0.0f )
		{
			forward = Vector( 1.0f, 0.0f, 0.0f );
		}

		Vector right( -forward.y, forward.x, 0.0f );
		if ( RandomInt( 0, 1 ) == 0 )
		{
			right *= -1.0f;
		}

		m_searchGoal = me->GetAbsOrigin() + right * 66.0f;
		m_path.Invalidate();
		m_repathTimer.Invalidate();
		return true;
	}

	CUtlVector< CNavArea * > nearbyVector;
	CollectSurroundingAreas( &nearbyVector, me->GetLastKnownArea(), 600.0f, me->GetLocomotionInterface()->GetStepHeight(), me->GetLocomotionInterface()->GetStepHeight() );

	if ( nearbyVector.Count() <= 0 )
		return false;

	int myTeam = me->GetTeamNumber();
	int enemyTeam = GetEnemyTeam( myTeam );

	CUtlVector< CTFNavArea * > candidateVector;
	for ( int i = 0; i < nearbyVector.Count(); ++i )
	{
		CTFNavArea *area = (CTFNavArea *)nearbyVector[i];
		if ( !area )
			continue;

		// Do not pick our current little patch. The point is to change sightlines.
		if ( area->GetCenter().DistToSqr( me->GetAbsOrigin() ) < 200.0f * 200.0f )
			continue;

		float myIncursion = area->GetIncursionDistance( myTeam );
		float enemyIncursion = area->GetIncursionDistance( enemyTeam );

		// Respect battle lines when both teams have valid incursion data.
		// If the enemy reaches this area much earlier than us, skip it.
		if ( myIncursion >= 0.0f && enemyIncursion >= 0.0f && myIncursion > enemyIncursion + 300.0f )
			continue;

		candidateVector.AddToTail( area );
	}

	if ( candidateVector.Count() <= 0 )
		return false;

	CTFNavArea *chosenArea = candidateVector[ RandomInt( 0, candidateVector.Count() - 1 ) ];
	m_searchGoal = chosenArea->GetRandomPoint();
	m_path.Invalidate();
	m_repathTimer.Invalidate();

	return true;
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerPatrolNest::UpdateSearchForFriendlyBuildings( CTFBot *me )
{
	CBaseObject *target = FindVisibleFriendlyNestTarget( me );
	if ( target )
	{
		m_targetBuilding = target;
		m_mode = PATROL_MODE_TARGET;
		m_hasArrived = false;
		m_hasSearchGoal = false;
		m_path.Invalidate();
		m_repathTimer.Invalidate();
		m_giveUpTimer.Start( 15.0f );
		return true;
	}

	if ( MyImportantBuildingsNeedMe( me ) )
		return false;

	if ( m_giveUpTimer.IsElapsed() )
		return false;

	if ( !m_hasSearchGoal )
		return false;

	const float searchRange = 50.0f;
	if ( me->GetAbsOrigin().DistToSqr( m_searchGoal ) <= searchRange * searchRange )
	{
		// After reaching a real search point, do a short left/right adjustment to open a new sightline,
		// then pick another safe nearby search point until the give-up timer expires.
		if ( m_searchPhase == SEARCH_PHASE_WANDER )
		{
			m_searchPhase = SEARCH_PHASE_SIDESTEP;
			m_hasSearchGoal = SelectSearchGoal( me );
			return m_hasSearchGoal;
		}

		m_searchPhase = SEARCH_PHASE_WANDER;
		m_hasSearchGoal = SelectSearchGoal( me );
		return m_hasSearchGoal;
	}

	if ( m_repathTimer.IsElapsed() )
	{
		m_repathTimer.Start( RandomFloat( 0.5f, 1.0f ) );

		CBaseCombatWeapon *shotgun = me->Weapon_GetSlot( TF_WPN_TYPE_PRIMARY );
		if ( shotgun )
		{
			me->Weapon_Switch( shotgun );
		}

		CTFBotPathCost cost( me, SAFEST_ROUTE );
		m_path.Compute( me, m_searchGoal, cost );
	}

	me->StartLookingAroundForEnemies();
	m_path.Update( me );
	return true;
}


//---------------------------------------------------------------------------------------------
CTFPlayer *CTFBotEngineerPatrolNest::FindFriendlyNonEngineerStandingOnBuilding( CTFBot *me, CBaseObject *target ) const
{
	if ( !me || !target )
		return NULL;

	Vector mins, maxs;
	target->CollisionProp()->WorldSpaceAABB( &mins, &maxs );

	CTFPlayer *bestPlayer = NULL;
	float bestRange = FLT_MAX;

	for ( int i = 1; i <= gpGlobals->maxClients; ++i )
	{
		CTFPlayer *player = ToTFPlayer( UTIL_PlayerByIndex( i ) );
		if ( !player )
			continue;

		if ( player == me )
			continue;

		if ( !player->IsAlive() )
			continue;

		if ( player->GetTeamNumber() != me->GetTeamNumber() )
			continue;

		if ( player->IsPlayerClass( TF_CLASS_ENGINEER ) )
			continue;

		Vector playerOrigin = player->GetAbsOrigin();
		const float expand = 24.0f;

		if ( playerOrigin.x < mins.x - expand || playerOrigin.x > maxs.x + expand )
			continue;

		if ( playerOrigin.y < mins.y - expand || playerOrigin.y > maxs.y + expand )
			continue;

		if ( playerOrigin.z < maxs.z - 8.0f || playerOrigin.z > maxs.z + 96.0f )
			continue;

		float range = me->GetDistanceBetween( player );
		if ( range < bestRange )
		{
			bestPlayer = player;
			bestRange = range;
		}
	}

	return bestPlayer;
}


//---------------------------------------------------------------------------------------------
Vector CTFBotEngineerPatrolNest::GetSpyCheckAimSpot( CTFBot *me, CBaseObject *target ) const
{
	CTFPlayer *suspect = FindFriendlyNonEngineerStandingOnBuilding( me, target );
	if ( suspect )
	{
		return suspect->WorldSpaceCenter();
	}

	Vector mins, maxs;
	target->CollisionProp()->WorldSpaceAABB( &mins, &maxs );

	Vector aimSpot = ( mins + maxs ) / 2.0f;
	aimSpot.z = maxs.z + 50.0f;

	return aimSpot;
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerPatrolNest::SpyCheckBuilding( CTFBot *me, CBaseObject *target )
{
	if ( !me || !target )
		return false;

	if ( m_spyCheckShotsRemaining <= 0 )
		return false;

	CBaseCombatWeapon *shotgun = me->Weapon_GetSlot( TF_WPN_TYPE_PRIMARY );
	if ( !shotgun )
		return false;

	me->Weapon_Switch( shotgun );

	Vector aimSpot = GetSpyCheckAimSpot( me, target );

	me->StopLookingAroundForEnemies();
	me->GetBodyInterface()->AimHeadTowards( aimSpot, IBody::CRITICAL, 0.25f, NULL, "Spy-check friendly Engineer nest" );

	if ( m_spyCheckShotTimer.IsElapsed() )
	{
		me->PressFireButton();
		--m_spyCheckShotsRemaining;
		m_spyCheckShotTimer.Start( RandomFloat( 0.30f, 0.50f ) );
	}

	return true;
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotEngineerPatrolNest::Update( CTFBot *me, float interval )
{
	if ( m_mode == PATROL_MODE_SEARCH_FOR_FRIENDLY_BUILDINGS )
	{
		if ( UpdateSearchForFriendlyBuildings( me ) )
			return Continue();

		return Done( "Finished searching for friendly Engineer buildings" );
	}

	CBaseObject *target = m_targetBuilding.Get();
	if ( !IsTargetValid( me, target ) )
		return Done( "No valid friendly nest patrol target" );

	if ( MyImportantBuildingsNeedMe( me ) )
		return Done( "My own nest needs me" );

	if ( FClassnameIs( target, "obj_sentrygun" ) )
	{
		CObjectSentrygun *friendlySentry = (CObjectSentrygun *)target;
		if ( !friendlySentry->IsMiniBuilding() && IsBuildingRecentlyAttacked( friendlySentry, 1.0f ) )
		{
			return SuspendFor( new CTFBotEngineerAssistFriendlyBuilding( friendlySentry, CTFBotEngineerAssistFriendlyBuilding::ASSIST_FRIENDLY_BUILDING_DEFEND ), "Defending patrolled friendly sentry" );
		}
	}

	const float patrolRange = 100.0f;
	if ( !m_hasArrived )
	{
		if ( m_giveUpTimer.IsElapsed() )
			return Done( "Taking too long to reach friendly nest" );

		if ( me->GetDistanceBetween( target ) > patrolRange )
		{
			if ( m_repathTimer.IsElapsed() )
			{
				m_repathTimer.Start( RandomFloat( 1.0f, 2.0f ) );

				CBaseCombatWeapon *shotgun = me->Weapon_GetSlot( TF_WPN_TYPE_PRIMARY );
				if ( shotgun )
				{
					me->Weapon_Switch( shotgun );
				}

				CTFBotPathCost cost( me, FASTEST_ROUTE );
				m_path.Compute( me, target->GetAbsOrigin(), cost );
			}

			m_path.Update( me );
			return Continue();
		}

		m_hasArrived = true;
		m_waitTimer.Start( RandomFloat( 8.0f, 15.0f ) );
	}

	if ( m_shouldSpyCheck && m_spyCheckShotsRemaining > 0 )
	{
		SpyCheckBuilding( me, target );
		return Continue();
	}

	me->StartLookingAroundForEnemies();

	if ( m_waitTimer.IsElapsed() )
		return Done( "Finished friendly nest patrol" );

	return Continue();
}


//---------------------------------------------------------------------------------------------
void CTFBotEngineerPatrolNest::OnEnd( CTFBot *me, Action< CTFBot > *nextAction )
{
	me->StartLookingAroundForEnemies();
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotEngineerPatrolNest::OnResume( CTFBot *me, Action< CTFBot > *interruptingAction )
{
	m_path.Invalidate();
	m_repathTimer.Invalidate();
	m_giveUpTimer.Reset();

	return Continue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotEngineerPatrolNest::OnStuck( CTFBot *me )
{
	m_path.Invalidate();
	return TryContinue();
}


//---------------------------------------------------------------------------------------------
QueryResultType CTFBotEngineerPatrolNest::ShouldHurry( const INextBot *me ) const
{
	return ANSWER_YES;
}
