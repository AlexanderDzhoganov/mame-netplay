Module.print = function (text) {
	window.jsmame_stdout(text, false)
}
Module.printErr = function (text) {
	window.jsmame_stdout(text, true)
}
