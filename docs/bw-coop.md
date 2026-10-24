# Pokemon Black co-op (bw-coop)

Experimental co-op presence for **Pokemon Black (US, revision 0, game code IRBO)**: when you and a friend are on
the same map, each of you sees the other's trainer walking around in your own game. Each player plays their own
save. Walk up to your friend's trainer, face them and press A to battle or trade, anywhere, from the start of the
game (no C-Gear needed). Trading works even with only one Pokemon in your party.

Part of the Co-op Anything project; evidence and addresses are in its `games/pokemon-black-white/NOTES.md`.

## Playing
1. Both players load their own copy of Pokemon Black (US) in this build. Use the default settings
   (JIT off). Other games and other versions/regions are ignored.
2. Host: System > Pokemon Black co-op > Host co-op..., pick a UDP port (default 47100).
3. Friend: System > Pokemon Black co-op > Join co-op..., enter `host-address:port`.
4. The host must be reachable on two UDP ports: the co-op port (default 47100) and 7064 (battles and trades run
   over emulated DS wireless). Forward both on the router, or use a private network such as Tailscale or ZeroTier
   and join with the host's address on that network.
5. Battle or trade: stand next to your friend's trainer, face them, press A. Both games open the battle/trade
   menu; pick the same option on both.
6. Optional autostart: set the environment variable `BWCOOP` to `host 47100` or `join 1.2.3.4 47100`.

## Known limitations
Your friend's trainer appears only when you are on the same map. Running/cycling speeds untested.
Exactly two players. Battles and trades slow down with ping, because the DS wireless protocol waits for a reply
round trip many times per second: in tests about 2.7x slower at 20 ms ping and 5.6x slower at 50 ms (they still
finish). Walking around together is not affected. Surfing and most maps are untested.
