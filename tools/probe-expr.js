(function () {
  var out = {};
  ['dark','light','blue'].forEach(function (t) {
    var e = document.querySelector('.sw[data-theme=' + t + ']');
    var cs = getComputedStyle(e);
    out[t] = { bg: cs.backgroundImage.slice(0, 60), border: cs.borderColor, ring: cs.boxShadow.slice(0, 40), attr: e.getAttribute('data-theme') };
  });
  out.page = document.documentElement.dataset.theme;
  return out;
})()