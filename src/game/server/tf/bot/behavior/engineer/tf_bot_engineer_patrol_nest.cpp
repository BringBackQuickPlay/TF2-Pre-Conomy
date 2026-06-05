//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_engineer_patrol_nest.cpp
// Engineer patrolling between his nest and a known friendly Engineer building

#include "cbase.h"
#include "nav_mesh.h"
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
CTFBotEngineerPatrolNest::CTFBotEngineerPatrolNest( CBaseObject *targetBuilding )
{
	m_targetBuilding = targetBuilding;
	m_homeSentry = NULL;
	m_hasArrived = false;
	m_shouldSpyCheck = false;
	m_spyCheckShotsRemaining = 0;
	m_spyCheckState = SPYCHECK_NONE;
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotEngineerPatrolNest::OnStart( CTFBot *me, Action< CTFBot > *priorAction )
{
	m_path.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );
	m_path.Invalidate();

	m_repathTimer.Invalidate();
	m_waitTimer.Invalidate();
	m_giveUpTimer.Start( 15.0f );
	m_spyCheckTimer.Invalidate();

	m_homeSentry = (CObjectSentrygun *)me->GetObjectOfType( OBJ_SENTRYGUN );
	m_hasArrived = false;
	m_shouldSpyCheck = RandomInt( 1, 100 ) <= 25;
	m_spyCheckShotsRemaining = m_shouldSpyCheck ? RandomInt( 1, 2 ) : 0;
	m_spyCheckState = m_shouldSpyCheck ? SPYCHECK_AIM : SPYCHECK_NONE;

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
bool CTFBotEngineerPatrolNest::UpdateSpyCheck( CTFBot *me, CBaseObject *target )
{
	if ( !me || !target )
		return false;

	if ( m_spyCheckState == SPYCHECK_NONE || m_spyCheckState == SPYCHECK_DONE )
		return false;

	CBaseCombatWeapon *shotgun = me->Weapon_GetSlot( TF_WPN_TYPE_PRIMARY );
	if ( !shotgun )
	{
		m_spyCheckState = SPYCHECK_DONE;
		return false;
	}

	me->Weapon_Switch( shotgun );
	me->StopLookingAroundForEnemies();
	me->GetBodyInterface()->AimHeadTowards( GetSpyCheckAimSpot( me, target ), IBody::CRITICAL, 0.25f, NULL, "Spy-check friendly Engineer nest" );

	if ( m_spyCheckState == SPYCHECK_AIM )
	{
		if ( !m_spyCheckTimer.HasStarted() )
		{
			m_spyCheckTimer.Start( 0.20f );
		}

		if ( m_spyCheckTimer.IsElapsed() )
		{
			m_spyCheckState = SPYCHECK_FIRE;
			m_spyCheckTimer.Invalidate();
		}

		return true;
	}

	if ( m_spyCheckState == SPYCHECK_FIRE )
	{
		me->PressFireButton();
		--m_spyCheckShotsRemaining;

		if ( m_spyCheckShotsRemaining <= 0 )
		{
			m_spyCheckState = SPYCHECK_DONE;
		}
		else
		{
			m_spyCheckState = SPYCHECK_WAIT_BETWEEN_SHOTS;
			m_spyCheckTimer.Start( RandomFloat( 0.35f, 0.55f ) );
		}

		return true;
	}

	if ( m_spyCheckState == SPYCHECK_WAIT_BETWEEN_SHOTS )
	{
		if ( m_spyCheckTimer.IsElapsed() )
		{
			m_spyCheckState = SPYCHECK_AIM;
			m_spyCheckTimer.Invalidate();
		}

		return true;
	}

	return false;
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotEngineerPatrolNest::Update( CTFBot *me, float interval )
{
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

	if ( m_shouldSpyCheck && m_spyCheckShotsRemaining > 0 && m_spyCheckState != SPYCHECK_DONE )
	{
		UpdateSpyCheck( me, target );
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
