//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_engineer_building.cpp
// At building location, constructing buildings
// Michael Booth, May 2010

#include "cbase.h"
#include "nav_mesh.h"
#include "tf_player.h"
#include "tf_obj.h"
#include "tf_obj_sentrygun.h"
#include "tf_obj_dispenser.h"
#include "tf_obj_teleporter.h"
#include "tf_gamerules.h"
#include "tf_weapon_builder.h"
#include "team_train_watcher.h"
#include "bot/tf_bot.h"
#include "bot/behavior/engineer/tf_bot_engineer_building.h"
#include "bot/behavior/engineer/tf_bot_engineer_move_to_build.h"
#include "bot/behavior/engineer/tf_bot_engineer_build_teleport_exit.h"
#include "bot/behavior/engineer/tf_bot_engineer_build_sentrygun.h"
#include "bot/behavior/engineer/tf_bot_engineer_build_dispenser.h"
#include "bot/behavior/tf_bot_attack.h"
#include "bot/behavior/tf_bot_get_ammo.h"
#include "bot/map_entities/tf_bot_hint_teleporter_exit.h"
#include "bot/map_entities/tf_bot_hint_sentrygun.h"
#include "NextBotUtil.h"


ConVar tf_bot_engineer_retaliate_range( "tf_bot_engineer_retaliate_range", "750", FCVAR_CHEAT, "If attacker who destroyed sentry is closer than this, attack. Otherwise, retreat" );
ConVar tf_bot_engineer_exit_near_sentry_range( "tf_bot_engineer_exit_near_sentry_range", "2500", FCVAR_CHEAT, "Maximum travel distance between a bot's Sentry gun and its Teleporter Exit" );
ConVar tf_bot_engineer_max_sentry_travel_distance_to_point( "tf_bot_engineer_max_sentry_travel_distance_to_point", "2500", FCVAR_CHEAT, "Maximum travel distance between a bot's Sentry gun and the currently contested point" );
ConVar tf_bot_engineer_improved_behavior( "tf_bot_engineer_improved_behavior", "0", FCVAR_CHEAT, "If set, Engineer bots do not treat mini-sentries as upgradeable sentries and should not get stuck trying to upgrade sentry/stuck not upgrading dispenser." );


extern ConVar tf_bot_path_lookahead_range;

const int MaxPlacementAttempts = 5;


//---------------------------------------------------------------------------------------------
CTFBotEngineerBuilding::CTFBotEngineerBuilding( void )
{
	m_sentryBuildHint = NULL;
}


//---------------------------------------------------------------------------------------------
CTFBotEngineerBuilding::CTFBotEngineerBuilding( CTFBotHintSentrygun *sentryBuildHint )
{
	m_sentryBuildHint = sentryBuildHint;
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot >	CTFBotEngineerBuilding::OnStart( CTFBot *me, Action< CTFBot > *priorAction )
{
	m_sentryTriesLeft = MaxPlacementAttempts;

	m_territoryRangeTimer.Invalidate();

	m_hasBuiltSentry = false;
	m_isSentryOutOfPosition = false;
	m_nearbyMetalStatus = NEARBY_METAL_UNKNOWN;

	m_friendlyBuildingTask = FRIENDLY_BUILDING_TASK_NONE;
	m_friendlyBuildingTarget = NULL;
	m_friendlyBuildingStartLevel = 0;
	m_spyCheckShotsRemaining = 0;

	m_friendlyBuildingRepathTimer.Invalidate();
	m_friendlyBuildingTaskTimer.Invalidate();
	m_spyCheckShotTimer.Invalidate();

	m_friendlyPatrolFeintGoal = vec3_origin;
	m_friendlyPatrolFeintReturnTarget = NULL;
	m_spyKilledPatrolBoostCount = 0;

	m_nextFriendlyPatrolTimer.Start( GetRandomizedPatrolDuration( 10.0f, 20.0f ) );
	m_nextFriendlyUpgradeTimer.Start( RandomFloat( 8.0f, 16.0f ) );
	m_nextFriendlySpyCheckTimer.Start( RandomFloat( 6.0f, 14.0f ) );

	return Continue();
}


//---------------------------------------------------------------------------------------------
// Attack the current target of our mini-sentry, but do not chase too far away from it.
bool CTFBotEngineerBuilding::TryAttackMiniSentryTarget( CTFBot *me, CObjectSentrygun *mySentry )
{
	if ( !me || !mySentry || !mySentry->IsMiniBuilding() )
		return false;

	CBaseEntity *targetEntity = mySentry->GetTarget();
	CTFPlayer *targetPlayer = ToTFPlayer( targetEntity );

	if ( !targetPlayer || !targetPlayer->IsAlive() )
		return false;

	if ( targetPlayer->GetTeamNumber() == me->GetTeamNumber() )
		return false;

	const float assistRange = 512.0f;
	Vector toTarget = targetPlayer->WorldSpaceCenter() - me->WorldSpaceCenter();

	if ( toTarget.LengthSqr() > assistRange * assistRange )
		return false;

	const float sentryLeashRange = 150.0f;
	float rangeToSentry = me->GetDistanceBetween( mySentry );

	if ( rangeToSentry > sentryLeashRange )
	{
		if ( m_repathTimer.IsElapsed() )
		{
			m_repathTimer.Start( RandomFloat( 0.5f, 1.0f ) );

			CTFBotPathCost cost( me, FASTEST_ROUTE );
			m_path.Compute( me, mySentry->GetAbsOrigin(), cost );
		}

		m_path.Update( me );
		return true;
	}

	CBaseCombatWeapon *shotgun = me->Weapon_GetSlot( TF_WPN_TYPE_PRIMARY );
	if ( !shotgun )
		return false;

	me->Weapon_Switch( shotgun );
	me->StopLookingAroundForEnemies();
	me->GetBodyInterface()->AimHeadTowards( targetPlayer->WorldSpaceCenter(), IBody::CRITICAL, 0.5f, NULL, "Attack my mini-sentry target" );
	me->PressFireButton();

	return true;
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerBuilding::IsBuildingRecentlyAttacked( CBaseObject *obj, float time ) const
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
// Important: our own mini-sentry is disposable.
// Damage to our own mini-sentry should not interrupt useful friendly-building work.
bool CTFBotEngineerBuilding::MyImportantBuildingsRecentlyAttacked( CTFBot *me, float time ) const
{
	CObjectSentrygun *mySentry = (CObjectSentrygun *)me->GetObjectOfType( OBJ_SENTRYGUN );
	CObjectDispenser *myDispenser = (CObjectDispenser *)me->GetObjectOfType( OBJ_DISPENSER );

	if ( myDispenser && IsBuildingRecentlyAttacked( myDispenser, time ) )
		return true;

	if ( mySentry && !mySentry->IsMiniBuilding() && IsBuildingRecentlyAttacked( mySentry, time ) )
		return true;

	return false;
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerBuilding::IsFriendlyBuildingCandidate( CTFBot *me, CBaseObject *obj ) const
{
	if ( !me || !obj )
		return false;

	if ( obj->GetTeamNumber() != me->GetTeamNumber() )
		return false;

	if ( obj->GetBuilder() == me )
		return false;

	return true;
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerBuilding::CanSeeBuilding( CTFBot *me, CBaseObject *obj ) const
{
	if ( !me || !obj )
		return false;

	trace_t result;
	UTIL_TraceLine( me->EyePosition(), obj->WorldSpaceCenter(), MASK_SOLID_BRUSHONLY, me, COLLISION_GROUP_NONE, &result );

	if ( result.fraction >= 1.0f )
		return true;

	if ( result.m_pEnt == obj )
		return true;

	return false;
}


//---------------------------------------------------------------------------------------------
void CTFBotEngineerBuilding::ReturnToOwnSentry( CTFBot *me )
{
	CObjectSentrygun *mySentry = (CObjectSentrygun *)me->GetObjectOfType( OBJ_SENTRYGUN );

	m_friendlyBuildingTask = FRIENDLY_BUILDING_TASK_RETURN_HOME;
	m_friendlyBuildingTarget = mySentry;
	m_friendlyBuildingRepathTimer.Invalidate();
}


//---------------------------------------------------------------------------------------------
void CTFBotEngineerBuilding::WorkOnBuilding( CTFBot *me, CBaseObject *target, const char *reason )
{
	if ( !me || !target )
		return;

	CBaseCombatWeapon *wrench = me->Weapon_GetSlot( TF_WPN_TYPE_MELEE );
	if ( wrench )
	{
		me->Weapon_Switch( wrench );
	}

	me->StopLookingAroundForEnemies();
	me->GetBodyInterface()->AimHeadTowards( target->WorldSpaceCenter(), IBody::CRITICAL, 1.0f, NULL, reason );
	me->PressFireButton();
}


//---------------------------------------------------------------------------------------------
// Everything is built, upgrade/maintain it
// TODO: Upgrade/maintain nearby friendly buildings, too.
void CTFBotEngineerBuilding::UpgradeAndMaintainBuildings( CTFBot *me )
{
	CObjectSentrygun *mySentry = (CObjectSentrygun *)me->GetObjectOfType( OBJ_SENTRYGUN );
	CObjectDispenser *myDispenser = (CObjectDispenser *)me->GetObjectOfType( OBJ_DISPENSER );

	if ( !mySentry )
	{
		return;
	}

	const float tooFarRange = 75.0f;

	if ( !myDispenser )
	{
		// just work on our sentry
		float rangeToSentry = me->GetDistanceBetween( mySentry );

		if ( rangeToSentry < 1.2f * tooFarRange )
		{
			// crouch both for cover behind our buildings, but also to slow us down so we hit our move goal more accurately
			me->PressCrouchButton();
		}

		if ( rangeToSentry > tooFarRange )
		{
			if ( m_repathTimer.IsElapsed() )
			{
				m_repathTimer.Start( RandomFloat( 1.0f, 2.0f ) );

				CTFBotPathCost cost( me, FASTEST_ROUTE );
				m_path.Compute( me, mySentry->GetAbsOrigin(), cost );
			}

			m_path.Update( me );
		}
		else
		{
			// we are in position - work on our buildings
			CBaseCombatWeapon *wrench = me->Weapon_GetSlot( TF_WPN_TYPE_MELEE );
			if ( wrench )
			{
				me->Weapon_Switch( wrench );
			}

			me->StopLookingAroundForEnemies();
			me->GetBodyInterface()->AimHeadTowards( mySentry->WorldSpaceCenter(), IBody::CRITICAL, 1.0f, NULL, "Work on my Sentry" );
			me->PressFireButton();
		}

		return;
	}

	// sit near both buildings
	Vector betweenMyBuildings = ( mySentry->GetAbsOrigin() + myDispenser->GetAbsOrigin() ) / 2.0f;

	// try to equalize distance between both
	float rangeToSentry = me->GetDistanceBetween( mySentry );
	float rangeToDispenser = me->GetDistanceBetween( myDispenser );

	const float equalTolerance = 25.0f;

	if ( rangeToSentry < 1.2f * tooFarRange && rangeToDispenser < 1.2f * tooFarRange )
	{
		// crouch both for cover behind our buildings, but also to slow us down so we hit our move goal more accurately
		me->PressCrouchButton();
	}

	if ( fabs( rangeToDispenser - rangeToSentry ) > equalTolerance || rangeToSentry > tooFarRange || rangeToDispenser > tooFarRange )
	{
		if ( m_repathTimer.IsElapsed() )
		{
			m_repathTimer.Start( RandomFloat( 1.0f, 2.0f ) );

			CTFBotPathCost cost( me, FASTEST_ROUTE );
			m_path.Compute( me, betweenMyBuildings, cost );
		}

		m_path.Update( me );
	}

	if ( rangeToSentry < tooFarRange || rangeToDispenser < tooFarRange )
	{
		// we are (nearly) in position - work on our buildings
		m_searchTimer.Invalidate();

		bool bImprovedMiniSentry = tf_bot_engineer_improved_behavior.GetBool() && mySentry->IsMiniBuilding();



		CBaseObject *workTarget = bImprovedMiniSentry ? NULL : mySentry;

		int iSentryLevel = mySentry->GetUpgradeLevel();
		bool bUpgradeSentry = iSentryLevel < 3;
		bool bRepairSentry = mySentry->GetTimeSinceLastInjury() < 1.0f || mySentry->GetHealth() < mySentry->GetMaxHealth();
		bool bSentryNeedsAmmo = false;
		int iDesiredDispenserLevel = iSentryLevel;

		if ( bImprovedMiniSentry )
		{
			bUpgradeSentry = false;
			bRepairSentry = false;
			bSentryNeedsAmmo = mySentry->IsAmmoLow( 0.50f );
			iDesiredDispenserLevel = 3;

			// If bImprovedMiniSentry, stand up again as there's little point to crouch behind a mini-sentry.
			me->ReleaseCrouchButton();
		}

		if ( mySentry->HasSapper() || mySentry->IsPlasmaDisabled() )
			workTarget = mySentry;
		else if ( myDispenser->HasSapper() || myDispenser->IsPlasmaDisabled() )
			workTarget = myDispenser;
		else if ( bRepairSentry )
			workTarget = mySentry;
		else if ( mySentry->IsBuilding() )
			workTarget = mySentry;
		else if ( myDispenser->IsBuilding() )
			workTarget = myDispenser;
		else if ( bUpgradeSentry )
			workTarget = mySentry;
		else if ( myDispenser->GetHealth() < myDispenser->GetMaxHealth() )
			workTarget = myDispenser;
		else if ( bSentryNeedsAmmo )
			workTarget = mySentry;
		else if ( myDispenser->GetUpgradeLevel() < iDesiredDispenserLevel )
			workTarget = myDispenser;

		if ( workTarget )
		{
			CBaseCombatWeapon *wrench = me->Weapon_GetSlot( TF_WPN_TYPE_MELEE );
			if ( wrench )
			{
				me->Weapon_Switch( wrench );
			}
			me->StopLookingAroundForEnemies();
			me->GetBodyInterface()->AimHeadTowards( workTarget->WorldSpaceCenter(), IBody::CRITICAL, 1.0f, NULL, "Work on my buildings" );
			me->PressFireButton();
		} 
		else
		{
			// We have no urgent work on our own important buildings.
			// Our own mini-sentry is disposable, so helping a friendly real sentry can take priority.
			if ( DefendFriendlyBuildings( me ) )
			{
				return;
			}

			// We did not have a workTarget, consider checking if our
			// mini-sentry has a target it's attacking and if it's close.
			// if true, then attack the target of our mini-sentry.
			if ( bImprovedMiniSentry && TryAttackMiniSentryTarget( me, mySentry ) )
			{
				return;
			}

			if ( UpgradeFriendlyBuildings( me ) )
			{
				return;
			}

			if ( PatrolBetweenMyAndFriendlyBuildings( me ) )
			{
				return;
			}

			if ( SpyCheckBuildings( me ) )
			{
				return;
			}

			CBaseCombatWeapon *shotgun = me->Weapon_GetSlot( TF_WPN_TYPE_PRIMARY );
			if ( shotgun )
			{
				me->Weapon_Switch( shotgun );
			}

			me->StartLookingAroundForEnemies();
		}
	}
}


//---------------------------------------------------------------------------------------------
float CTFBotEngineerBuilding::GetRandomizedPatrolDuration( float minTime, float maxTime ) const
{
	if ( RandomInt( 1, 100 ) <= 20 )
	{
		float offset = (float)RandomInt( 1, 3 );

		if ( RandomInt( 0, 1 ) == 0 )
		{
			minTime -= offset;
			maxTime -= offset;
		}
		else
		{
			minTime += offset;
			maxTime += offset;
		}
	}

	if ( minTime < 1.0f )
		minTime = 1.0f;

	if ( maxTime < minTime )
		maxTime = minTime;

	return RandomFloat( minTime, maxTime );
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerBuilding::BeginPatrolFeint( CTFBot *me, CBaseObject *realGoal, CBaseObject *returnTarget )
{
	if ( !me || !realGoal || !returnTarget )
		return false;

	if ( RandomInt( 1, 100 ) > 15 )
		return false;

	m_friendlyPatrolFeintGoal = me->GetAbsOrigin() + 0.5f * ( realGoal->GetAbsOrigin() - me->GetAbsOrigin() );
	m_friendlyPatrolFeintReturnTarget = returnTarget;

	m_friendlyBuildingTask = FRIENDLY_BUILDING_TASK_PATROL_FEINT;
	m_friendlyBuildingRepathTimer.Invalidate();

	return true;
}


//---------------------------------------------------------------------------------------------
CBaseObject *CTFBotEngineerBuilding::FindClosestFriendlyPlayerForSpyCheck( CTFBot *me ) const
{
	CBaseObject *bestPlayer = NULL;
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

		float range = me->GetDistanceBetween( player );
		if ( range < bestRange )
		{
			bestPlayer = (CBaseObject *)player;
			bestRange = range;
		}
	}

	return bestPlayer;
}


//---------------------------------------------------------------------------------------------
CBaseObject *CTFBotEngineerBuilding::FindVisibleFriendlyPatrolBuilding( CTFBot *me ) const
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

		if ( !CanSeeBuilding( me, sentry ) )
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

		if ( !CanSeeBuilding( me, dispenser ) )
			continue;

		float range = me->GetDistanceBetween( dispenser );
		if ( range < bestDispenserRange )
		{
			bestDispenser = dispenser;
			bestDispenserRange = range;
		}
	}

	if ( bestSentry )
		return bestSentry;

	return bestDispenser;
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerBuilding::PatrolBetweenMyAndFriendlyBuildings( CTFBot *me )
{
	CObjectSentrygun *mySentry = (CObjectSentrygun *)me->GetObjectOfType( OBJ_SENTRYGUN );

	if ( !mySentry )
		return false;

	if ( m_friendlyBuildingTask != FRIENDLY_BUILDING_TASK_NONE &&
		 m_friendlyBuildingTask != FRIENDLY_BUILDING_TASK_PATROL_TO_FRIENDLY &&
		 m_friendlyBuildingTask != FRIENDLY_BUILDING_TASK_PATROL_WAIT &&
		 m_friendlyBuildingTask != FRIENDLY_BUILDING_TASK_RETURN_HOME &&
		 m_friendlyBuildingTask != FRIENDLY_BUILDING_TASK_PATROL_FEINT )
	{
		return false;
	}

	if ( MyImportantBuildingsRecentlyAttacked( me, 1.0f ) )
	{
		ReturnToOwnSentry( me );
		return false;
	}

	if ( m_friendlyBuildingTask == FRIENDLY_BUILDING_TASK_NONE )
	{
		if ( !m_nextFriendlyPatrolTimer.IsElapsed() )
			return false;

		// 20% chance to skip this patrol cycle entirely.
		if ( RandomInt( 1, 100 ) <= 20 )
		{
			m_nextFriendlyPatrolTimer.Start( GetRandomizedPatrolDuration( 10.0f, 20.0f ) );
			return false;
		}

		CBaseObject *target = FindVisibleFriendlyPatrolBuilding( me );
		if ( !target )
		{
			m_nextFriendlyPatrolTimer.Start( GetRandomizedPatrolDuration( 10.0f, 20.0f ) );
			return false;
		}

		// 15% chance to fake leaving the nest, run halfway, then return.
		if ( BeginPatrolFeint( me, target, mySentry ) )
			return true;

		m_friendlyBuildingTask = FRIENDLY_BUILDING_TASK_PATROL_TO_FRIENDLY;
		m_friendlyBuildingTarget = target;
		m_friendlyBuildingRepathTimer.Invalidate();
	}

	if ( m_friendlyBuildingTask == FRIENDLY_BUILDING_TASK_PATROL_FEINT )
	{
		CBaseObject *returnTarget = m_friendlyPatrolFeintReturnTarget.Get();
		if ( !returnTarget )
		{
			m_friendlyBuildingTask = FRIENDLY_BUILDING_TASK_NONE;
			m_friendlyBuildingTarget = NULL;
			m_friendlyPatrolFeintReturnTarget = NULL;
			m_nextFriendlyPatrolTimer.Start( GetRandomizedPatrolDuration( 10.0f, 20.0f ) );
			return false;
		}

		const float feintRange = 75.0f;

		if ( me->GetAbsOrigin().DistTo( m_friendlyPatrolFeintGoal ) > feintRange )
		{
			if ( m_friendlyBuildingRepathTimer.IsElapsed() )
			{
				m_friendlyBuildingRepathTimer.Start( RandomFloat( 0.5f, 1.0f ) );

				CBaseCombatWeapon *shotgun = me->Weapon_GetSlot( TF_WPN_TYPE_PRIMARY );
				if ( shotgun )
				{
					me->Weapon_Switch( shotgun );
				}

				CTFBotPathCost cost( me, FASTEST_ROUTE );
				m_path.Compute( me, m_friendlyPatrolFeintGoal, cost );
			}

			m_path.Update( me );
			return true;
		}

		m_friendlyBuildingTask = FRIENDLY_BUILDING_TASK_RETURN_HOME;
		m_friendlyBuildingTarget = returnTarget;
		m_friendlyPatrolFeintReturnTarget = NULL;
		m_friendlyBuildingRepathTimer.Invalidate();
		return true;
	}

	CBaseObject *target = m_friendlyBuildingTarget.Get();
	if ( !target )
	{
		m_friendlyBuildingTask = FRIENDLY_BUILDING_TASK_NONE;
		m_nextFriendlyPatrolTimer.Start( GetRandomizedPatrolDuration( 10.0f, 20.0f ) );
		return false;
	}

	if ( m_friendlyBuildingTask == FRIENDLY_BUILDING_TASK_PATROL_TO_FRIENDLY )
	{
		const float patrolRange = 100.0f;

		if ( me->GetDistanceBetween( target ) > patrolRange )
		{
			if ( m_friendlyBuildingRepathTimer.IsElapsed() )
			{
				m_friendlyBuildingRepathTimer.Start( RandomFloat( 1.0f, 2.0f ) );

				CBaseCombatWeapon *shotgun = me->Weapon_GetSlot( TF_WPN_TYPE_PRIMARY );
				if ( shotgun )
				{
					me->Weapon_Switch( shotgun );
				}

				CTFBotPathCost cost( me, FASTEST_ROUTE );
				m_path.Compute( me, target->GetAbsOrigin(), cost );
			}

			// While going to patrol, sometimes stop and fire 2 shots at the closest friendly ally.
			//int allySpyCheckChance = ( m_spyKilledPatrolBoostCount > 0 ) ? 35 : 15;
			int allySpyCheckChance = 15;
			if ( RandomInt( 1, 100 ) <= allySpyCheckChance )
			{
				CBaseObject *ally = FindClosestFriendlyPlayerForSpyCheck( me );
				if ( ally )
				{
					CBaseCombatWeapon *shotgun = me->Weapon_GetSlot( TF_WPN_TYPE_PRIMARY );
					if ( shotgun )
					{
						me->Weapon_Switch( shotgun );
					}

					me->StopLookingAroundForEnemies();
					me->GetBodyInterface()->AimHeadTowards( ally->WorldSpaceCenter(), IBody::CRITICAL, 0.25f, NULL, "Spy-check friendly ally while patrolling" );
					me->PressFireButton();
					me->PressFireButton();

					//if ( m_spyKilledPatrolBoostCount > 0 )
					//	--m_spyKilledPatrolBoostCount;

					return true;
				}
			}

			m_path.Update( me );
			return true;
		}

		m_friendlyBuildingTask = FRIENDLY_BUILDING_TASK_PATROL_WAIT;
		m_friendlyBuildingTaskTimer.Start( GetRandomizedPatrolDuration( 6.0f, 10.0f ) );
		return true;
	}

	if ( m_friendlyBuildingTask == FRIENDLY_BUILDING_TASK_PATROL_WAIT )
	{
		me->StartLookingAroundForEnemies();

		if ( FClassnameIs( target, "obj_sentrygun" ) )
		{
			CObjectSentrygun *friendlySentry = (CObjectSentrygun *)target;

			if ( !friendlySentry->IsMiniBuilding() && IsBuildingRecentlyAttacked( friendlySentry, 1.0f ) )
			{
				if ( me->GetDistanceBetween( friendlySentry ) < me->GetDistanceBetween( mySentry ) )
				{
					m_friendlyBuildingTask = FRIENDLY_BUILDING_TASK_DEFEND;
					m_friendlyBuildingTaskTimer.Start( GetRandomizedPatrolDuration( 4.0f, 6.0f ) );
					return true;
				}
			}
		}

		if ( !m_friendlyBuildingTaskTimer.IsElapsed() )
			return true;

		// 15% chance to fake going home, run halfway, then return to the friendly building.
		if ( BeginPatrolFeint( me, mySentry, target ) )
			return true;

		ReturnToOwnSentry( me );
		return true;
	}

	if ( m_friendlyBuildingTask == FRIENDLY_BUILDING_TASK_RETURN_HOME )
	{
		const float homeRange = 100.0f;

		if ( me->GetDistanceBetween( target ) > homeRange )
		{
			if ( m_friendlyBuildingRepathTimer.IsElapsed() )
			{
				m_friendlyBuildingRepathTimer.Start( RandomFloat( 1.0f, 2.0f ) );

				CTFBotPathCost cost( me, FASTEST_ROUTE );
				m_path.Compute( me, target->GetAbsOrigin(), cost );
			}

			m_path.Update( me );
			return true;
		}

		m_friendlyBuildingTask = FRIENDLY_BUILDING_TASK_NONE;
		m_friendlyBuildingTarget = NULL;
		m_nextFriendlyPatrolTimer.Start( GetRandomizedPatrolDuration( 10.0f, 20.0f ) );
		return false;
	}

	return false;
}


//---------------------------------------------------------------------------------------------
CBaseObject *CTFBotEngineerBuilding::FindVisibleFriendlyUpgradeBuilding( CTFBot *me ) const
{
	// upgrade target finder here
	return NULL;
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerBuilding::UpgradeFriendlyBuildings( CTFBot *me )
{
	// upgrade behaviour here
	return NULL;
}


//---------------------------------------------------------------------------------------------
CObjectSentrygun *CTFBotEngineerBuilding::FindFriendlySentryNeedingDefense( CTFBot *me ) const
{
	// defense target finder here
	return NULL;
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerBuilding::DefendFriendlyBuildings( CTFBot *me )
{
	// defense behaviour here
	return NULL;
}


//---------------------------------------------------------------------------------------------
CBaseObject *CTFBotEngineerBuilding::FindSpyCheckBuilding( CTFBot *me ) const
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

		if ( sentry->GetTeamNumber() != me->GetTeamNumber() )
			continue;

		// There's little point to spychecking a mini-sentry.
		if ( sentry->IsMiniBuilding() )
			continue;

		if ( !CanSeeBuilding( me, sentry ) )
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

		if ( dispenser->GetTeamNumber() != me->GetTeamNumber() )
			continue;

		if ( !CanSeeBuilding( me, dispenser ) )
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
		if ( RandomInt( 1, 100 ) <= 25 ) // 25% chance to spycheck the sentry if we found both.
			return bestSentry;

		return bestDispenser;
	}

	if ( bestSentry )
		return bestSentry;

	return bestDispenser;

	
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerBuilding::SpyCheckBuildings( CTFBot *me )
{
	if ( MyImportantBuildingsRecentlyAttacked( me, 1.0f ) )
		return false;

	if ( m_friendlyBuildingTask != FRIENDLY_BUILDING_TASK_NONE &&
		 m_friendlyBuildingTask != FRIENDLY_BUILDING_TASK_SPYCHECK )
	{
		return false;
	}

	if ( m_friendlyBuildingTask != FRIENDLY_BUILDING_TASK_SPYCHECK )
	{
		if ( !m_nextFriendlySpyCheckTimer.IsElapsed() )
			return false;

		CBaseObject *target = FindSpyCheckBuilding( me );
		if ( !target )
		{
			m_nextFriendlySpyCheckTimer.Start( RandomFloat( 6.0f, 14.0f ) );
			return false;
		}

		m_friendlyBuildingTask = FRIENDLY_BUILDING_TASK_SPYCHECK;
		m_friendlyBuildingTarget = target;
		m_spyCheckShotsRemaining = RandomInt( 1, 2 );
		m_spyCheckShotTimer.Invalidate();
	}

	CBaseObject *target = m_friendlyBuildingTarget.Get();
	if ( !target )
	{
		m_friendlyBuildingTask = FRIENDLY_BUILDING_TASK_NONE;
		m_nextFriendlySpyCheckTimer.Start( RandomFloat( 6.0f, 14.0f ) );
		return false;
	}

	CBaseCombatWeapon *shotgun = me->Weapon_GetSlot( TF_WPN_TYPE_PRIMARY );
	if ( shotgun )
	{
		me->Weapon_Switch( shotgun );
	}

	Vector aimSpot = target->WorldSpaceCenter();
	aimSpot.z += 50.0f;

	me->GetBodyInterface()->AimHeadTowards( aimSpot, IBody::CRITICAL, 0.25f, NULL, "Spy-check friendly building" );

	if ( m_spyCheckShotTimer.IsElapsed() )
	{
		me->PressFireButton();

		--m_spyCheckShotsRemaining;
		m_spyCheckShotTimer.Start( RandomFloat( 0.25f, 0.45f ) );
	}

	if ( m_spyCheckShotsRemaining <= 0 )
	{
		m_friendlyBuildingTask = FRIENDLY_BUILDING_TASK_NONE;
		m_friendlyBuildingTarget = NULL;
		m_nextFriendlySpyCheckTimer.Start( RandomFloat( 8.0f, 18.0f ) );

		me->StartLookingAroundForEnemies();
		return true;
	}

	return true;
}



//---------------------------------------------------------------------------------------------
bool CTFBotEngineerBuilding::IsMetalSourceNearby( CTFBot *me ) const
{
	CUtlVector< CNavArea * > nearbyVector;
	CollectSurroundingAreas( &nearbyVector, me->GetLastKnownArea(), 2000.0f, me->GetLocomotionInterface()->GetStepHeight(), me->GetLocomotionInterface()->GetStepHeight() );

	for( int i=0; i<nearbyVector.Count(); ++i )
	{
		CTFNavArea *area = (CTFNavArea *)nearbyVector[i];
		if ( area->HasAttributeTF( TF_NAV_HAS_AMMO ) )
		{
			return true;
		}

		// this assumes all spawn rooms have resupply cabinets
		if ( me->GetTeamNumber() == TF_TEAM_RED && area->HasAttributeTF( TF_NAV_SPAWN_ROOM_RED ) )
		{
			return true;
		}

		if ( me->GetTeamNumber() == TF_TEAM_BLUE && area->HasAttributeTF( TF_NAV_SPAWN_ROOM_BLUE ) )
		{
			return true;
		}
	}

	return false;
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerBuilding::CheckIfSentryIsOutOfPosition( CTFBot *me ) const
{
	CObjectSentrygun *mySentry = (CObjectSentrygun *)me->GetObjectOfType( OBJ_SENTRYGUN );

	if ( !mySentry )
	{
		return false;
	}

	// payload
	if ( TFGameRules()->GetGameType() == TF_GAMETYPE_ESCORT )
	{
		CTeamTrainWatcher *trainWatcher;

		if ( me->GetTeamNumber() == TF_TEAM_BLUE )
		{
			trainWatcher = TFGameRules()->GetPayloadToPush( me->GetTeamNumber() );
		}
		else
		{
			trainWatcher = TFGameRules()->GetPayloadToBlock( me->GetTeamNumber() );
		}

		if ( trainWatcher )
		{
			float sentryDistanceAlongPath;
			trainWatcher->ProjectPointOntoPath( mySentry->GetAbsOrigin(), NULL, &sentryDistanceAlongPath );

			const float behindTrainTolerance = SENTRY_MAX_RANGE;
			return ( trainWatcher->GetTrainDistanceAlongTrack() > sentryDistanceAlongPath + behindTrainTolerance );
		}
	}

	// control points
	mySentry->UpdateLastKnownArea();
	CNavArea *sentryArea = mySentry->GetLastKnownArea();

	CTeamControlPoint *point = me->GetMyControlPoint();
	if ( point )
	{
		CTFNavArea *pointArea = TheTFNavMesh()->GetControlPointCenterArea( point->GetPointIndex() );

		if ( sentryArea && pointArea )
		{
			CTFBotPathCost cost( me, FASTEST_ROUTE );
			if ( NavAreaTravelDistance( sentryArea, pointArea, cost, tf_bot_engineer_max_sentry_travel_distance_to_point.GetFloat() ) < 0 &&
				 NavAreaTravelDistance( pointArea, sentryArea, cost, tf_bot_engineer_max_sentry_travel_distance_to_point.GetFloat() ) < 0 )
			{
				return true;
			}
		}
	}

	return false;
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot >	CTFBotEngineerBuilding::Update( CTFBot *me, float interval )
{
	CObjectSentrygun *mySentry = (CObjectSentrygun *)me->GetObjectOfType( OBJ_SENTRYGUN );
	CObjectDispenser *myDispenser = (CObjectDispenser *)me->GetObjectOfType( OBJ_DISPENSER );
	CObjectTeleporter *myTeleportEntrance = (CObjectTeleporter *)me->GetObjectOfType( OBJ_TELEPORTER, MODE_TELEPORTER_ENTRANCE );
	CObjectTeleporter *myTeleportExit = (CObjectTeleporter *)me->GetObjectOfType( OBJ_TELEPORTER, MODE_TELEPORTER_EXIT );

	bool isUnderAttack = ( me->GetTimeSinceLastInjury() < 1.0f );
	isUnderAttack |= ( mySentry && ( mySentry->HasSapper() || mySentry->IsPlasmaDisabled() ) );
	isUnderAttack |= ( myDispenser && ( myDispenser->HasSapper() || myDispenser->IsPlasmaDisabled() ) );

	me->StartLookingAroundForEnemies();

	// try to build a Sentry
	if ( !mySentry )
	{
		m_nearbyMetalStatus = NEARBY_METAL_UNKNOWN;

		// react to nearby threats if our sentry is down
		const CKnownEntity *threat = me->GetVisionInterface()->GetPrimaryKnownThreat();
		if ( threat && threat->IsVisibleRecently() )
		{
			me->EquipBestWeaponForThreat( threat );
		}

		if ( !m_hasBuiltSentry && m_sentryTriesLeft > 0 )
		{
			--m_sentryTriesLeft;

			if ( m_sentryBuildHint )
			{
				return SuspendFor( new CTFBotEngineerBuildSentryGun( m_sentryBuildHint ), "Building a Sentry at a hint location" );
			}

			return SuspendFor( new CTFBotEngineerBuildSentryGun, "Building a Sentry" );
		}
		else
		{
			// can't build a Sentry here - pick a new place
			return ChangeTo( new CTFBotEngineerMoveToBuild, "Couldn't find a place to build" );
		}
	}

	// I have a Sentry
	m_hasBuiltSentry = true;

	if ( m_sentryBuildHint != NULL && !m_sentryBuildHint->IsEnabled() )
	{
		// our hint has been disabled and no longer has influence on our behavior
		m_sentryBuildHint = NULL;
	}

	// periodically check that our Sentry is still near the contested point
	if ( m_sentryBuildHint == NULL || !m_sentryBuildHint->IsSticky() )
	{
		if ( !m_isSentryOutOfPosition && m_territoryRangeTimer.IsElapsed() )
		{
			m_territoryRangeTimer.Start( RandomFloat( 3.0f, 5.0f ) );

			m_isSentryOutOfPosition = CheckIfSentryIsOutOfPosition( me );
		}

		if ( m_isSentryOutOfPosition )
		{
			// the point has moved, only keep sentry as long as it keeps attacking
			if ( mySentry->GetTimeSinceLastFired() > 10.0f )
			{
				mySentry->DetonateObject();

				// if we built here because of a hint, disable that hint so we don't use it and rebuild here again
				if ( m_sentryBuildHint != NULL )
				{
					inputdata_t dummy;
					m_sentryBuildHint->InputDisable( dummy );

					m_sentryBuildHint = NULL;
				}

				if ( myDispenser )
				{
					myDispenser->DetonateObject();
				}

				if ( myTeleportExit )
				{
					myTeleportExit->DetonateObject();
				}

				me->SpeakConceptIfAllowed( MP_CONCEPT_PLAYER_MOVEUP );

				return ChangeTo( new CTFBotEngineerMoveToBuild, "Need to move my gear closer to the point!" );
			}
		}
	}

	// if my dispenser is too far away from my sentry, destroy and rebuild it next update
	// @TODO: Flag hint-built entities for a larger range
	const float maxSeparation = 500.0f;
	if ( myDispenser )
	{
		if ( ( mySentry->GetAbsOrigin() - myDispenser->GetAbsOrigin() ).IsLengthGreaterThan( maxSeparation ) )
		{
			myDispenser->DestroyObject();
			myDispenser = NULL;
		}
	}

	// build up the sentry all the way if there is a metal source nearby
	if ( mySentry->GetUpgradeLevel() < 3 && ( !tf_bot_engineer_improved_behavior.GetBool() || !mySentry->IsMiniBuilding() ) )
	{
		if ( m_nearbyMetalStatus == NEARBY_METAL_UNKNOWN )
		{
			m_nearbyMetalStatus = IsMetalSourceNearby( me ) ? NEARBY_METAL_EXISTS : NEARBY_METAL_NONE;
		}

		if ( m_nearbyMetalStatus == NEARBY_METAL_EXISTS )
		{
			UpgradeAndMaintainBuildings( me );
			return Continue();
		}
	}

/*
	if ( myTeleportExit )
	{
		// if my teleporter exit is too far away from my sentry, destroy and rebuild it next update
		if ( ( mySentry->GetAbsOrigin() - myTeleportExit->GetAbsOrigin() ).IsLengthGreaterThan( maxSeparation ) )
		{
			myTeleportExit->DestroyObject();
			myTeleportExit = NULL;
		}
	}
*/

	// try to build a Dispenser
	const float dispenserRebuildInterval = 10.0f;
	if ( myDispenser )
	{
		// don't rebuild immediately after building is destroyed
		m_dispenserRetryTimer.Start( dispenserRebuildInterval );
	}
	else if ( m_dispenserRetryTimer.IsElapsed() && !isUnderAttack )
	{
		m_dispenserRetryTimer.Start( dispenserRebuildInterval );

		return SuspendFor( new CTFBotEngineerBuildDispenser, "Building a Dispenser" );
	}

	// try to build a Teleporter Exit
	const float exitRebuildInterval = 30.0f;
	if ( myTeleportExit )
	{
		// don't rebuild immediately after building is destroyed
		m_teleportExitRetryTimer.Start( exitRebuildInterval );
	}
	else if ( m_teleportExitRetryTimer.IsElapsed() && myTeleportEntrance && !isUnderAttack )
	{
		m_teleportExitRetryTimer.Start( exitRebuildInterval );

		// we need to build a teleporter exit yet
		if ( m_sentryBuildHint != NULL )
		{
			// if there are teleporter exit hints, find the closest one to our sentry and use it
			CUtlVector< CBaseEntity * > hintVector;
			CTFBotHintTeleporterExit *hint = NULL;
			while( ( hint = (CTFBotHintTeleporterExit *)gEntList.FindEntityByClassname( hint, "bot_hint_teleporter_exit" ) ) != NULL )
			{
				if ( hint->IsEnabled() && hint->InSameTeam( me ) )
				{
					hintVector.AddToTail( hint );
				}
			}

			if ( hintVector.Count() > 0 )
			{
				mySentry->UpdateLastKnownArea();
				CBaseEntity *closeHint = SelectClosestEntityByTravelDistance( me, hintVector, mySentry->GetLastKnownArea(), tf_bot_engineer_exit_near_sentry_range.GetFloat() );

				if ( closeHint )
				{
					return SuspendFor( new CTFBotEngineerBuildTeleportExit( closeHint->GetAbsOrigin(), closeHint->GetAbsAngles().y ), "Building teleporter exit at nearby hint" );
				}
			}
		}
		else if ( me->IsRangeLessThan( mySentry, 300.0f ) )
		{
			// drop a teleporter exit near our sentry
			return SuspendFor( new CTFBotEngineerBuildTeleportExit(), "Building teleporter exit" );
		}
	}

	// everything is built - maintain them
	UpgradeAndMaintainBuildings( me );

	return Continue();
}


//---------------------------------------------------------------------------------------------
void CTFBotEngineerBuilding::OnEnd( CTFBot *me, Action< CTFBot > *nextAction )
{
	me->StartLookingAroundForEnemies();
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotEngineerBuilding::OnResume( CTFBot *me, Action< CTFBot > *interruptingAction )
{
	return Continue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotEngineerBuilding::OnTerritoryLost( CTFBot *me, int territoryID )
{
	return TryContinue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotEngineerBuilding::OnTerritoryCaptured( CTFBot *me, int territoryID )
{
	return TryContinue();
}
