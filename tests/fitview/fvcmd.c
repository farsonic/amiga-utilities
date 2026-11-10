/*
 * fvcmd.c - FitView's test helper for the emulator runs: sends one command to FitView's ARexx
 * port and waits for the answer, without RexxMast (the command is a plain string in rm_Args[0],
 * which is all FitView reads). Waits up to WAIT seconds (default 20) for the port to appear.
 *   fvcmd PORT/A,WAIT/K/N,COMMAND/F       e.g. fvcmd FITVIEW.1 CHANGEWINDOW 10 20 400 300
 * Prints "fvcmd: COMMAND -> rc" (rc -1: no such port).
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <rexx/storage.h>
#include <rexx/errors.h>
#include <proto/exec.h>
#include <proto/dos.h>

int main(void)
{
  static const char tmpl[] = "PORT/A,WAIT/K/N,COMMAND/F";
  LONG a[3] = { 0, 0, 0 }, secs = 20, i;
  struct RDArgs *rd = ReadArgs((STRPTR)tmpl, a, NULL);
  struct MsgPort *reply, *port = NULL;
  struct RexxMsg rm;
  const char *cmd;
  if (!rd) { PrintFault(IoErr(), (STRPTR)"fvcmd"); return 10; }
  if (a[1]) secs = *(LONG *)a[1];
  cmd = a[2] ? (const char *)a[2] : "";
  if (!(reply = CreateMsgPort())) { FreeArgs(rd); return 20; }
  memset(&rm, 0, sizeof rm);
  rm.rm_Node.mn_ReplyPort = reply;
  rm.rm_Node.mn_Length = sizeof rm;
  rm.rm_Action = RXCOMM;
  rm.rm_Args[0] = (STRPTR)cmd;
  for (i = 0; i <= secs * 5; i++) {
    Forbid();
    if ((port = FindPort((STRPTR)a[0]))) PutMsg(port, &rm.rm_Node);
    Permit();
    if (port) break;
    Delay(10);
  }
  if (port) {
    WaitPort(reply);
    GetMsg(reply);
    printf("fvcmd: %s -> %ld\n", cmd, (long)rm.rm_Result1);
  } else printf("fvcmd: %s -> -1 (no port %s)\n", cmd, (char *)a[0]);
  DeleteMsgPort(reply);
  FreeArgs(rd);
  return port ? 0 : 5;
}
