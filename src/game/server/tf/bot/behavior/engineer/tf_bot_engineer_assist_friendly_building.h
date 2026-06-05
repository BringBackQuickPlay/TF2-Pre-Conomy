//========= Copyright Valve Corporation, All rights reserved. ============//
// tf_bot_engineer_assist_friendly_building.h
// Engineer assisting nearby friendly Engineer buildings

#ifndef TF_BOT_ENGINEER_ASSIST_FRIENDLY_BUILDING_H
#define TF_BOT_ENGINEER_ASSIST_FRIENDLY_BUILDING_H

#include "Path/NextBotPathFollow.h"

class CBaseObject;
class CObjectSentrygun;
class CObjectDispenser;

class CTFBotEngineerAssistFriendlyBuilding : public Action< CTFBot >
{
public:
	enum AssistMode
	{
		ASSIST_FRIENDLY_BUILDING_AUTO = 0,
		ASSIST_FRIENDLY_BUILDING_UPGRADE,
		ASSIST_FRIENDLY_BUILDING_REPAIR,
		ASSIST_FRIENDLY_BUILDING_DEFEND,
		ASSIST_FRIENDLY_BUILDING_DESTROY_STICKIES
	};

	CTFBotEngineerAssistFriendlyBuilding( CBaseObject *target, AssistMode mode = ASSIST_FRIENDLY_BUILDING_AUTO );

	virtual ActionResult< CTFBot >	OnStart( CTFBot *me, Action< CTFBot > *priorAction );
	virtual ActionResult< CTFBot >	Update( CTFBot *me, float interval );
	virtual void					OnEnd( CTFBot *me, Action< CTFBot > *nextAction );
	virtual ActionResult< CTFBot >	OnResume( CTFBot *me, Action< CTFBot > *interruptingAction );
	virtual EventDesiredResult< CTFBot > OnStuck( CTFBot *me );

	virtual QueryResultType	ShouldHurry( const INextBot *me ) const;

	virtual const char *GetName( void ) const	{ return "EngineerAssistFriendlyBuilding"; };

private:
	CHandle< CBaseObject > m_target;
	AssistMode m_mode;

	PathFollower m_path;
	CountdownTimer m_repathTimer;
	CountdownTimer m_giveUpTimer;
	CountdownTimer m_recentAttackGraceTimer;
	CountdownTimer m_dispenserCheckTimer;
	CountdownTimer m_stickyCheckTimer;
	CountdownTimer m_shotTimer;
	CountdownTimer m_getAmmoTimer;

	int m_startUpgradeLevel;
	bool m_hasArrived;
	bool m_hasWhackedNearbyDispenser;

	bool IsTargetValid( CTFBot *me, CBaseObject *target ) const;
	bool MyImportantBuildingsNeedMe( CTFBot *me ) const;
	bool IsBuildingRecentlyAttacked( CBaseObject *obj, float time ) const;
	bool IsRepairNeeded( CBaseObject *obj ) const;
	bool IsUpgradeNeeded( CBaseObject *obj ) const;
	bool NeedsMetalToWorkOnBuilding( CBaseObject *obj ) const;
	bool ShouldCollectMetalForFriendlyBuildingWork( CTFBot *me, CBaseObject *target ) const;

	CObjectDispenser *FindNearbyDamagedFriendlyDispenser( CTFBot *me, CBaseObject *target ) const;
	CBaseEntity *FindStickybombNearBuilding( CTFBot *me, CBaseObject *target ) const;
	CTFPlayer *FindVisibleInvulnerableThreat( CTFBot *me ) const;

	void WorkOnBuilding( CTFBot *me, CBaseObject *target, const char *reason );
	bool ShootTargetWithShotgun( CTFBot *me, CBaseEntity *target, const char *reason );
};

#endif // TF_BOT_ENGINEER_ASSIST_FRIENDLY_BUILDING_H
