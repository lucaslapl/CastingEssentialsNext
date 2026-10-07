# CastingEssentialsNext

A fork of CastingEssentialsRed.

CastingEssentialsNext is the follow-up of CastingEssentialsRed that supports 64-bit TF2, with all it's original functionality.

CastingEssentials by Pazer, based on StatusSpec by tsc.
CastingEssentialsRed by Phoenix Red.
CastingEssentialsNext by resin (James Puleo)

CastingEssentials is a Team Fortress 2 client plugin that enhances the experience of both casters and viewers. Reimplementation/fork of [StatusSpec](https://github.com/fwdcp/StatusSpec) by [tsc](https://github.com/thesupremecommander).

## Changes on this fork

On top of [upstream](https://github.com/drunderscore/CastingEssentialsNext), this fork adds:

- **ETF2L aliases** - the Player Aliases module can automatically fetch player names from [ETF2L](https://etf2l.org) and use them as aliases. Enable with `ce_playeraliases_etf2l 1`; `ce_playeraliases_etf2l_refresh` re-fetches names for the connected players and `ce_playeraliases_etf2l_reset` clears the on-disk cache (`tf/cfg/ce_playeraliases_etf2l_cache.cfg`).
