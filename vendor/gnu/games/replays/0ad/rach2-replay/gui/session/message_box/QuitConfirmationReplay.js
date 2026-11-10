/**
 * Ends a visual replay at its last turn, in place of public's dialog that
 * asks what to do next, so that a run that times the replay's frames ends by
 * itself. On standard output it prints the turns the simulation ran, the
 * replay's turn count when it was played whole, and the microseconds from
 * the session's start, after the loading screen, to the replay's end, which
 * tell the run which of its frames were the replay's.
 */
class QuitConfirmationReplay
{
	constructor()
	{
		this.start = Engine.GetMicroseconds();
		Engine.GetGUIObjectByName("session").onReplayFinished = this.finish.bind(this);
	}

	finish()
	{
		let elapsed = Engine.GuiInterfaceCall("GetSimulationState").timeElapsed;
		print("rach2-replay: end: turn " + Math.round(elapsed / 200) + " time " + elapsed +
			" us " + Math.round(Engine.GetMicroseconds() - this.start) + "\n");
		Engine.Exit();
	}
}
