JK2PUGBot
=========

This is a simple IRC bot I wrote for organizing pickup games in Star
Wars Jedi Knight II: Jedi Outcast community. You are free to use and
modify it according to the GPL-3 license.

It's written in C and has tiny memory footprint so it's well suited
for running on embedded devices. I run it on my OpenWRT router.

Features
--------

* Recommend servers and query q3-based ones.
* Support multiple pickup lists.
* !add !remove !who !promote commands accept multiple arguments.
* Track nick changes and autoremove on PART and QUIT.
* Auth with Q.

Configuration
-------------

Basic configurable options are at the top of the jk2pugbot.c file. You
need to dwell into the code a little more to change other things.

Compilation
-----------

It's recommended to use -O2, or -Os or at the very least
-foptimize-sibling-calls GCC flag.

    gcc -std=gnu99 -O2 jk2pugbot.c -o jk2pugbot

