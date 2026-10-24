# Pokemon Black co-op (bw-coop)

Experimental co-op presence for **Pokemon Black (US, revision 0, game code IRBO)**: when you and a friend are on
the same map, each of you sees the other's trainer walking around in your own game. Each player plays their own
save; nothing else is shared yet (story, items and battles are still separate).

Part of the Co-op Anything project; evidence and addresses are in its `games/pokemon-black-white/NOTES.md`.

## Playing
1. Both players load their own copy of Pokemon Black (US) in this build. Use the default settings
   (JIT off). Other games and other versions/regions are ignored.
2. Host: System > Pokemon Black co-op > Host co-op..., pick a UDP port (default 47100).
3. Friend: System > Pokemon Black co-op > Join co-op..., enter `host-address:port`.
4. The host's UDP port must be reachable: forward it on the router, or use a private network such as Tailscale
   or ZeroTier and join with the host's address on that network.
5. Optional autostart: set the environment variable `BWCOOP` to `host 47100` or `join 1.2.3.4 47100`.

## Known limitations
Your friend's trainer appears only when you are on the same map. Running/cycling speeds untested.
Battles, menus, surfing and most maps are untested. Both trainers use your partner's own sprite id.
