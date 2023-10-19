const JoulescopeDriver = require("../lib/binding.js");
const assert = require("assert");

function testBasic() {
    var context = JoulescopeDriver.initialize();
    JoulescopeDriver.finalize(context);
}

assert.doesNotThrow(testBasic, undefined, "testBasic threw an expection");

console.log("Tests passed- everything looks OK!");
