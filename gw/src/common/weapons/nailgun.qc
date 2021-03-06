void WeaponNailgun_SpawnNail(vector pos, vector dir, entity player);
void WeaponNailgun_OnTouch();
void WeaponNailgun_NailExplode(vector position, vector forward, entity ignored, entity target);

#ifdef SSQC
float WeaponNailgun_SendEntity(entity ent, float flags);
#endif

void WeaponNailgun_Init(entity missile)
{
	missile.classtype = CLASSTYPE_PROJECTILE;
	missile.movetype = MOVETYPE_FLYMISSILE;
	missile.solid = SOLID_SLIDEBOX;
	missile.classname = CLASS_NAIL;
	missile.effects |= EF_NOSHADOW | EF_FULLBRIGHT;
	missile.touch = WeaponNailgun_OnTouch;

	setmodel(missile, MDL_NAIL);
	setsize(missile, VEC_ORIGIN, VEC_ORIGIN);

	missile.dimension_solid = DIMENSION_PROJECTILE;
	missile.dimension_hit = DIMENSION_ALL & (~DIMENSION_PROJECTILE);

#ifdef SSQC
	missile.SendEntity = WeaponNailgun_SendEntity;
#endif
#ifdef CSQC
	missile.drawmask = MASK_ENGINE;
#endif
}

int WeaponNailgun_Fire(vector forward)
{
	vector offset = self.view_ofs;

	WeaponNailgun_SpawnNail((self.origin + (forward * 16)) + offset, forward, self);

	clientsound(self, CHAN_WEAPON, SND_WEAPON_NAILGUN, 1, ATTN_NORM, 1, SOUNDFLAG_FOLLOW);

	return 1;
}

void WeaponNailgun_SpawnNail(vector pos, vector dir, entity player)
{
	entity missile = spawn();
	missile.owner = player;
	missile.velocity = dir * NAILGUN_SPEED;
	missile.angles = vectoangles(dir);
	missile.origin = pos;

	setorigin(missile, missile.origin);
	WeaponNailgun_Init(missile);
}

#ifdef CSQC
void WeaponNailgun_Update(float isnew)
{
	ReadProjectile(isnew, WeaponNailgun_Init);
}
#endif
#ifdef SSQC
float WeaponNailgun_SendEntity(entity ent, float flags)
{
	WriteProjectile(MSG_ENTITY, SENDENTITY_NAIL);
	return 1;
}
#endif

void WeaponNailgun_OnTouch()
{
	//TODO: remove after 30 seconds or so
	if (other == self.owner)
		return;
	else if (other.solid == SOLID_CORPSE)
		return;

	if (pointcontents(self.origin) == CONTENT_SKY)
	{
		// projectile went out of bounds, just remove it
		remove(self);
		return;
	}

	WeaponNailgun_NailExplode(self.origin, normalize(self.velocity), self, other);

	remove(self);
}

void WeaponNailgun_NailExplode(vector position, vector forward, entity ignored, entity target)
{
	addRadialForce(position, NAILGUN_FORCE, NAILGUN_RADIUS, ignored, target, NAILGUN_FORCE_DISTANCE_POWER);
	addRadialDamage(position, NAILGUN_DAMAGE, NAILGUN_RADIUS, ignored, target, NAILGUN_DAMAGE_DISTANCE_POWER);

	if (target != world)
	{
		// direct hit with other entity, apply force and damage directly
		applyEntityForce(other, normalize(target.origin - position) * NAILGUN_FORCE, self);
		applyEntityDamage(other, NAILGUN_DAMAGE, self);
	}

/*#ifdef SSQC
	writeByte(MSG_BROADCAST, SVC_TEMPENTITY);
	writeByte(MSG_BROADCAST, TE_EXPLOSION);
	writeVector(MSG_BROADCAST, position - (18 * forward));
#endif*/
}
