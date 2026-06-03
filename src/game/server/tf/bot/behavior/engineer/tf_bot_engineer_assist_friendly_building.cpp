//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_engineer_assist_friendly_building.cpp
// Engineer assisting nearby friendly Engineer buildings

#include "cbase.h"
#include "nav_mesh.h"
#include "tf_player.h"
#include "tf_obj.h"
#include "tf_obj_sentrygun.h"
#include "tf_obj_dispenser.h"
#include "tf_obj_teleporter.h"
#include "tf_gamerules.h"
#include "tf_weaponbase.h"
#include "bot/tf_bot.h"
#include "bot/behavior/engineer/tf_bot_engineer_assist_friendly_building.h"
#include "bot/behavior/tf_bot_get_ammo.h"
#include "NextBotUtil.h"


//---------------------------------------------------------------------------------------------
CTFBotEngineerAssistFriendlyBuilding::CTFBotEngineerAssistFriendlyBuilding( CBaseObject *target, AssistMode mode )
{
	m_target = target;
	m_mode = mode;
	m_startUpgradeLevel = 0;
	m_hasArrived = false;
	m_hasWhackedNearbyDispenser = false;
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotEngineerAssistFriendlyBuilding::OnStart( CTFBot *me, Action< CTFBot > *priorAction )
{
	m_path.SetMinLookAheadDistance( me->GetDesiredPathLookAheadRange() );
	m_path.Invalidate();

	m_repathTimer.Invalidate();
	m_giveUpTimer.Start( 12.0f );
	m_recentAttackGraceTimer.Invalidate();
	m_dispenserCheckTimer.Invalidate();
	m_stickyCheckTimer.Invalidate();
	m_shotTimer.Invalidate();
	m_getAmmoTimer.Invalidate();

	m_hasArrived = false;
	m_hasWhackedNearbyDispenser = false;

	CBaseObject *target = m_target.Get();
	m_startUpgradeLevel = target ? target->GetUpgradeLevel() : 0;

	return Continue();
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerAssistFriendlyBuilding::IsBuildingRecentlyAttacked( CBaseObject *obj, float time ) const
{
	if ( !obj )
		return false;

	if ( obj->HasSapper() || obj->IsPlasmaDisabled() )
		return true;

	if ( obj->GetTimeSinceLastInjury() < time )
		return true;

	return false;
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerAssistFriendlyBuilding::IsRepairNeeded( CBaseObject *obj ) const
{
	if ( !obj )
		return false;

	if ( obj->HasSapper() || obj->IsPlasmaDisabled() )
		return true;

	if ( obj->GetHealth() < obj->GetMaxHealth() )
		return true;

	if ( obj->IsBuilding() )
		return true;

	return false;
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerAssistFriendlyBuilding::IsUpgradeNeeded( CBaseObject *obj ) const
{
	if ( !obj )
		return false;

	if ( FClassnameIs( obj, "obj_sentrygun" ) )
	{
		CObjectSentrygun *sentry = (CObjectSentrygun *)obj;
		if ( sentry->IsMiniBuilding() )
			return false;
	}

	return obj->GetUpgradeLevel() < 3;
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerAssistFriendlyBuilding::ShouldCollectMetalForFriendlyBuildingWork( CTFBot *me, CBaseObject *target ) const
{
	if ( !me || !target )
		return false;

	if ( !IsRepairNeeded( target ) && !IsUpgradeNeeded( target ) )
		return false;

	// Metal is represented as Engineer ammo. The existing Engineer actions use IsAmmoLow()
	// before suspending for CTFBotGetAmmo, so follow the same convention here.
	return me->IsAmmoLow();
}


//---------------------------------------------------------------------------------------------
bool CTFBotEngineerAssistFriendlyBuilding::IsTargetValid( CTFBot *me, CBaseObject *target ) const
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
bool CTFBotEngineerAssistFriendlyBuilding::MyImportantBuildingsNeedMe( CTFBot *me ) const
{
	CObjectSentrygun *mySentry = (CObjectSentrygun *)me->GetObjectOfType( OBJ_SENTRYGUN );
	CObjectDispenser *myDispenser = (CObjectDispenser *)me->GetObjectOfType( OBJ_DISPENSER );

	if ( myDispenser && ( IsRepairNeeded( myDispenser ) || IsBuildingRecentlyAttacked( myDispenser, 1.0f ) ) )
		return true;

	if ( mySentry && !mySentry->IsMiniBuilding() && ( IsRepairNeeded( mySentry ) || IsBuildingRecentlyAttacked( mySentry, 1.0f ) ) )
		return true;

	return false;
}


//---------------------------------------------------------------------------------------------
CObjectDispenser *CTFBotEngineerAssistFriendlyBuilding::FindNearbyDamagedFriendlyDispenser( CTFBot *me, CBaseObject *target ) const
{
	if ( !me || !target )
		return NULL;

	const float nearbyRange = 512.0f;
	CObjectDispenser *bestDispenser = NULL;
	float bestRange = FLT_MAX;

	CBaseEntity *ent = NULL;
	while ( ( ent = gEntList.FindEntityByClassname( ent, "obj_dispenser" ) ) != NULL )
	{
		CObjectDispenser *dispenser = dynamic_cast< CObjectDispenser * >( ent );
		if ( !dispenser )
			continue;

		if ( dispenser->GetTeamNumber() != me->GetTeamNumber() )
			continue;

		if ( dispenser->GetHealth() >= dispenser->GetMaxHealth() * 0.75f )
			continue;

		float range = dispenser->GetAbsOrigin().DistTo( target->GetAbsOrigin() );
		if ( range > nearbyRange )
			continue;

		if ( range < bestRange )
		{
			bestDispenser = dispenser;
			bestRange = range;
		}
	}

	return bestDispenser;
}


//---------------------------------------------------------------------------------------------
CBaseEntity *CTFBotEngineerAssistFriendlyBuilding::FindStickybombNearBuilding( CTFBot *me, CBaseObject *target ) const
{
	if ( !me || !target )
		return NULL;

	const char *stickyClassNames[] =
	{
		"tf_projectile_pipe_remote",
		"tf_projectile_pipe"
	};

	const float stickyRange = 150.0f;

	for ( int whichClass = 0; whichClass < ARRAYSIZE( stickyClassNames ); ++whichClass )
	{
		CBaseEntity *ent = NULL;
		while ( ( ent = gEntList.FindEntityByClassname( ent, stickyClassNames[ whichClass ] ) ) != NULL )
		{
			if ( ent->GetTeamNumber() == me->GetTeamNumber() )
				continue;

			if ( ent->GetAbsOrigin().DistTo( target->GetAbsOrigin() ) <= stickyRange )
				return ent;
		}
	}

	return NULL;
}


//---------------------------------------------------------------------------------------------
CTFPlayer *CTFBotEngineerAssistFriendlyBuilding::FindVisibleInvulnerableThreat( CTFBot *me ) const
{
	CUtlVector< CKnownEntity > knownVector;
	me->GetVisionInterface()->CollectKnownEntities( &knownVector );

	for ( int i = 0; i < knownVector.Count(); ++i )
	{
		if ( knownVector[i].IsObsolete() || !knownVector[i].IsVisibleRecently() )
			continue;

		CTFPlayer *player = ToTFPlayer( knownVector[i].GetEntity() );
		if ( !player )
			continue;

		if ( player->GetTeamNumber() == me->GetTeamNumber() )
			continue;

		if ( player->m_Shared.InCond( TF_COND_INVULNERABLE ) )
			return player;
	}

	return NULL;
}


//---------------------------------------------------------------------------------------------
void CTFBotEngineerAssistFriendlyBuilding::WorkOnBuilding( CTFBot *me, CBaseObject *target, const char *reason )
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
bool CTFBotEngineerAssistFriendlyBuilding::ShootTargetWithShotgun( CTFBot *me, CBaseEntity *target, const char *reason )
{
	if ( !me || !target )
		return false;

	CBaseCombatWeapon *shotgun = me->Weapon_GetSlot( TF_WPN_TYPE_PRIMARY );
	if ( !shotgun )
		return false;

	me->Weapon_Switch( shotgun );
	me->StopLookingAroundForEnemies();
	me->GetBodyInterface()->AimHeadTowards( target->WorldSpaceCenter(), IBody::CRITICAL, 0.25f, NULL, reason );

	if ( m_shotTimer.IsElapsed() )
	{
		me->PressFireButton();
		m_shotTimer.Start( RandomFloat( 0.25f, 0.45f ) );
	}

	return true;
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotEngineerAssistFriendlyBuilding::Update( CTFBot *me, float interval )
{
	CBaseObject *target = m_target.Get();
	if ( !IsTargetValid( me, target ) )
		return Done( "No valid friendly building to assist" );

	if ( MyImportantBuildingsNeedMe( me ) )
		return Done( "My own important buildings need me" );

	CObjectSentrygun *targetSentry = FClassnameIs( target, "obj_sentrygun" ) ? (CObjectSentrygun *)target : NULL;
	if ( targetSentry && targetSentry->IsMiniBuilding() )
	{
		me->StartLookingAroundForEnemies();
		return Done( "Ignoring friendly mini-sentry as a work target" );
	}

	if ( ShouldCollectMetalForFriendlyBuildingWork( me, target ) )
	{
		if ( m_getAmmoTimer.IsElapsed() && CTFBotGetAmmo::IsPossible( me ) )
		{
			m_getAmmoTimer.Start( 1.0f );
			return SuspendFor( new CTFBotGetAmmo, "Need metal to assist friendly building" );
		}

		// Do not stand next to a friendly building swinging forever with no metal.
		if ( !CTFBotGetAmmo::IsPossible( me ) )
		{
			return Done( "Need metal but cannot find any" );
		}
	}

	const float workRange = 75.0f;
	if ( me->GetDistanceBetween( target ) > workRange )
	{
		if ( m_giveUpTimer.IsElapsed() )
			return Done( "Taking too long to reach friendly building" );

		if ( m_repathTimer.IsElapsed() )
		{
			m_repathTimer.Start( RandomFloat( 1.0f, 2.0f ) );

			CBaseCombatWeapon *shotgun = me->Weapon_GetSlot( TF_WPN_TYPE_PRIMARY );
			if ( shotgun )
			{
				me->Weapon_Switch( shotgun );
			}

			CTFBotPathCost cost( me, ( targetSentry && IsBuildingRecentlyAttacked( targetSentry, 1.0f ) ) ? SAFEST_ROUTE : FASTEST_ROUTE );
			m_path.Compute( me, target->GetAbsOrigin(), cost );
		}

		m_path.Update( me );
		return Continue();
	}

	m_hasArrived = true;

	if ( targetSentry && IsBuildingRecentlyAttacked( targetSentry, 1.0f ) )
	{
		m_recentAttackGraceTimer.Start( 5.0f );
	}

	if ( targetSentry )
	{
		CTFPlayer *uberThreat = FindVisibleInvulnerableThreat( me );
		if ( uberThreat )
		{
			if ( uberThreat->IsPlayerClass( TF_CLASS_DEMOMAN ) )
			{
				return Done( "Enemy invulnerable Demoman approaching friendly sentry" );
			}

			// Heavy Uber: old-school hold-the-line behavior. Keep repairing until the sentry dies or the action ends.
			if ( uberThreat->IsPlayerClass( TF_CLASS_HEAVYWEAPONS ) )
			{
				WorkOnBuilding( me, targetSentry, "Hold friendly sentry against invulnerable Heavy" );
				return Continue();
			}
		}
	}

	if ( targetSentry && ( IsBuildingRecentlyAttacked( targetSentry, 1.0f ) || !m_recentAttackGraceTimer.IsElapsed() ) )
	{
		if ( m_dispenserCheckTimer.IsElapsed() )
		{
			m_dispenserCheckTimer.Start( 2.0f );

			CObjectDispenser *nearbyDispenser = FindNearbyDamagedFriendlyDispenser( me, targetSentry );
			if ( nearbyDispenser )
			{
				WorkOnBuilding( me, nearbyDispenser, "Briefly repair nearby friendly dispenser" );
				return Continue();
			}
		}

		WorkOnBuilding( me, targetSentry, "Defend friendly sentry" );
		return Continue();
	}

	if ( !targetSentry || ( targetSentry->GetHealth() >= targetSentry->GetMaxHealth() * 0.60f && targetSentry->GetTimeSinceLastInjury() > 0.5f ) )
	{
		if ( m_stickyCheckTimer.IsElapsed() )
		{
			m_stickyCheckTimer.Start( RandomFloat( 0.3f, 0.6f ) );

			CBaseEntity *sticky = FindStickybombNearBuilding( me, target );
			if ( sticky )
			{
				if ( ShootTargetWithShotgun( me, sticky, "Destroy stickybombs near friendly building" ) )
					return Continue();
			}
		}
	}

	if ( IsRepairNeeded( target ) )
	{
		WorkOnBuilding( me, target, "Repair friendly building" );
		return Continue();
	}

	if ( IsUpgradeNeeded( target ) )
	{
		if ( target->GetUpgradeLevel() > m_startUpgradeLevel )
			return Done( "Upgraded friendly building one level" );

		WorkOnBuilding( me, target, "Upgrade friendly building one level" );
		return Continue();
	}

	return Done( "Friendly building no longer needs assistance" );
}


//---------------------------------------------------------------------------------------------
void CTFBotEngineerAssistFriendlyBuilding::OnEnd( CTFBot *me, Action< CTFBot > *nextAction )
{
	me->StartLookingAroundForEnemies();
}


//---------------------------------------------------------------------------------------------
ActionResult< CTFBot > CTFBotEngineerAssistFriendlyBuilding::OnResume( CTFBot *me, Action< CTFBot > *interruptingAction )
{
	m_path.Invalidate();
	m_repathTimer.Invalidate();
	m_giveUpTimer.Reset();

	return Continue();
}


//---------------------------------------------------------------------------------------------
EventDesiredResult< CTFBot > CTFBotEngineerAssistFriendlyBuilding::OnStuck( CTFBot *me )
{
	m_path.Invalidate();
	return TryContinue();
}


//---------------------------------------------------------------------------------------------
QueryResultType CTFBotEngineerAssistFriendlyBuilding::ShouldHurry( const INextBot *me ) const
{
	return ANSWER_YES;
}
