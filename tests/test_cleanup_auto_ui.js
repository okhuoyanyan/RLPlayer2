// Run the cleanup suite against the shared full-production DOM/loopback harness.
// Each invocation still creates its own random profile and never starts a player.
process.argv.push('--suite','cleanup');
require('./test_quickstart_preference.js');
