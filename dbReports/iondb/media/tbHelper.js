/**20120624-is-it-ready
$(document).ready(function() {
	$("#configure").mouseover(function(){
    	$("#gear").removeClass().addClass("gear-active");
    }).mouseout(function(){
		$("#gear").removeClass().addClass("gear-inactive");    			
    }); 
});
*/

$(document).ready(function(){
	if ($(".dynamic-navbar").length <= 0) return
	
	//$(".dynamic-navbar").empty();
	$.each($("a[dynamic-navbar-section]"), function(element) {
		var href = $.trim($(this).attr("href"));
		var title = $.trim($(this).text());
		var section = '<li><a href="' + href + '" class="navitem">' + title + '</a></li>';
		$(".dynamic-navbar").append(section);
	});
});

$(document).ready(function(){
    var $win = $(window)
      , $nav = $('.page-nav')
      , navTop = $('.page-nav').length && $('.page-nav').offset().top + 10
      , isFixed = 0

    // processScroll()

    // hack sad times - holdover until rewrite for 2.1
    $nav.on('click', function () {
      if (!isFixed) setTimeout(function () {  $win.scrollTop($win.scrollTop() - 150) }, 10)
    })

    // $win.on('scroll', processScroll)
    // 
    // function processScroll() {
    //   var i, scrollTop = $win.scrollTop()
    //   if (scrollTop >= navTop && !isFixed) {
    //     isFixed = 1
    //     $nav.addClass('page-nav-fixed')
    //   } else if (scrollTop <= navTop && isFixed) {
    //     isFixed = 0
    //     $nav.removeClass('page-nav-fixed')
    //   }
    // }
});

$(document).ready(function(){
	//Enables body to be scanned for any tooltips occurrences
	if ($.fn.tooltip){
	    $('body').tooltip({
	      selector: "*[rel=tooltip]"
	    });
   	}
   	if ($.fn.dropdown) {
   		$('.dropdown-toggle').dropdown();
   	}
});

function getParameterByName(name) {
  name = name.replace(/[\[]/, "\\\[").replace(/[\]]/, "\\\]");
  var regexS = "[\\?&]" + name + "=([^&#]*)";
  var regex = new RegExp(regexS);
  var results = regex.exec(window.location.search);
  if(results == null)
    return "";
  else
    return decodeURIComponent(results[1].replace(/\+/g, " "));
}