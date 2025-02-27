<?php
require_once('./common_lib.inc');
require_once('./plugins.php.inc');
require_once('./util.inc');

if (isset($REDIRECT_HTTPS) && $REDIRECT_HTTPS && !isSslConnection() && GetSetting('prefer_https')) {
    $location = 'https://' . $_SERVER['HTTP_HOST'] . $_SERVER['REQUEST_URI'];
    header('HTTP/1.1 301 Moved Permanently');
    header('Location: ' . $location);
    exit(0);
}

// if any query parameter includes a .., exit right away - likely a hack attempt
foreach($_REQUEST as $key => $val) {
    if (is_string($val) && strlen($val) && strpos($val, '/../') !== false) {
        header('HTTP/1.1 403 Forbidden');
        echo "<html><body>Sorry, the request was blocked, please contact us for details";
        echo "<br>" . htmlspecialchars($key) . " - " . htmlspecialchars($val) . "</body></html>";
        exit(0);
    }
}
// fast exit for Nagios monitoring scripts
if (array_key_exists('HTTP_USER_AGENT', $_SERVER) &&
    strlen($_SERVER['HTTP_USER_AGENT']) &&
    stripos($_SERVER['HTTP_USER_AGENT'], 'nagios') !== false) {
    echo "OK";
    exit(0);
}


// shared initializiation/loading code
set_time_limit(300);
if (!array_key_exists('debug', $_REQUEST) && (!isset($debug) || !$debug)) {
    error_reporting(0);
}
umask(0);
date_default_timezone_set('UTC');
extract($_POST, EXTR_SKIP|EXTR_PREFIX_ALL|EXTR_REFS, 'req');
extract($_GET, EXTR_SKIP|EXTR_PREFIX_ALL|EXTR_REFS, 'req');

// add a CORS header
header('Access-Control-Allow-Origin: *');

// set up a global curl context that can be used for keep-alive connections (particularly when doing bulk relays)
if (function_exists('curl_init')) {
    $CURL_CONTEXT = curl_init();
    if ($CURL_CONTEXT !== false) {
        curl_setopt($CURL_CONTEXT, CURLOPT_RETURNTRANSFER, true);
        curl_setopt($CURL_CONTEXT, CURLOPT_FAILONERROR, true);
        curl_setopt($CURL_CONTEXT, CURLOPT_FOLLOWLOCATION, true);
        curl_setopt($CURL_CONTEXT, CURLOPT_CONNECTTIMEOUT, 30);
        curl_setopt($CURL_CONTEXT, CURLOPT_DNS_CACHE_TIMEOUT, 600);
        curl_setopt($CURL_CONTEXT, CURLOPT_MAXREDIRS, 10);
        curl_setopt($CURL_CONTEXT, CURLOPT_TIMEOUT, 600);
        curl_setopt($CURL_CONTEXT, CURLOPT_SSL_VERIFYHOST, 0);
        curl_setopt($CURL_CONTEXT, CURLOPT_SSL_VERIFYPEER, 0);
    }
} else
    $CURL_CONTEXT = false;

if (isset($_SERVER["REMOTE_ADDR"]) && $_SERVER["REMOTE_ADDR"] == '127.0.0.1' && isset($_REQUEST['addr'])) {
  $_SERVER["REMOTE_ADDR"] = $_REQUEST['addr'];
}
if (function_exists('apache_request_headers')) {
    $headers = apache_request_headers();
    if (array_key_exists('X-Forwarded-For', $headers)){
        $_SERVER["HTTP_X_FORWARDED_FOR"] = $headers['X-Forwarded-For'];
    }
}
if (isset($_SERVER["HTTP_X_FORWARDED_FOR"])) {
  $forwarded = explode(',',$_SERVER["HTTP_X_FORWARDED_FOR"]);
  if (isset($forwarded) && is_array($forwarded) && count($forwarded)) {
    $forwarded_ip = trim(end($forwarded));
    if (strlen($forwarded_ip) && $forwarded_ip != "127.0.0.1")
        $_SERVER["REMOTE_ADDR"] = $forwarded_ip;
  }
}
if (isset($_SERVER["HTTP_FASTLY_CLIENT_IP"]))
  $_SERVER["REMOTE_ADDR"] = $_SERVER["HTTP_FASTLY_CLIENT_IP"];

if (array_key_exists('HTTP_MOD_REWRITE', $_SERVER) && $_SERVER['HTTP_MOD_REWRITE'] == 'On') {
    define('FRIENDLY_URLS', true);
    define('VER_TIMELINE', '28/');       // version of the timeline javascript
} else {
    define('FRIENDLY_URLS', false);
    define('VER_TIMELINE', '');       // Leave the timeline version empty
}

$userIsBot = false;
if (array_key_exists('HTTP_USER_AGENT', $_SERVER) &&
    strlen($_SERVER['HTTP_USER_AGENT']) &&
    preg_match('/robot|spider|crawler|indexer|WeSEE|Googlebot|YandexBot|Twitterbot|SemrushBot|Slackbot|Slack-ImgProxy|bingbot|SEOdiver|EPiServer|BlockBit|Wget|^$/i', $_SERVER['HTTP_USER_AGENT'])) {
  $userIsBot = true;
}

// constants
define('VER_WEBPAGETEST', '19.04');   // webpagetest version
define('VER_CSS', 74);                // version of the sitewide css file
define('VER_JS', 33);                 // version of the sitewide javascript file
define('VER_JS_TEST', 41);            // version of the javascript specific to the test pages
define('VER_JS_RUNNING', 1);          // version of the javascript specific to the test running status page
define('UNKNOWN_TIME', -1);           // Value used as a flag for an unknown time.
                                      // To find code that fails to check that a time
                                      // is unknown, change this constant to a large
                                      // negative number.

// SEO stuff
$page_keywords = array('WebPageTest','Website Speed Test','Page Speed');
$page_description = "Run a free website speed test from around the globe using real browsers at consumer connection speeds with detailed optimization recommendations.";

$tempDir = './tmp';
if (!is_dir($tempDir))
    mkdir($tempDir, 0777, true);
$tempDir = realpath($tempDir) . '/';

if (isset($_REQUEST['embed'])) {
    define('EMBED', true);
    define('BARE_UI', true);
    define('NAV_NO_SHARE', true);
} elseif (isset($_REQUEST['bare'])) {
    define('BARE_UI', true);
    define('NAV_NO_SHARE', true);
    $noanalytics = true;
    if (extension_loaded('newrelic'))
      @newrelic_disable_autorum();
}
// load the install-specific settings
$settings = parse_ini_file('./settings/settings.ini');
if (!isset($settings) || !is_array($settings))
    $settings = array();
if (!array_key_exists('nogzip', $settings))
    $settings['nogzip'] = false;
if (!array_key_exists('nolinks', $settings))
    $settings['nolinks'] = 0;
if (!array_key_exists('shard_tests', $settings))
    $settings['shard_tests'] = 0;
$max_load = GetSetting('render_max_load');

$privateInstall = true;
if (array_key_exists('HTTP_HOST', $_SERVER) &&
    stristr($_SERVER['HTTP_HOST'] , 'httparchive.webpagetest.org') === false &&
    stristr($_SERVER['HTTP_HOST'] , 'webpagetest.org') !== false) {
    $privateInstall = false;
}

// override some settings from the URI
if (isset($_REQUEST['bulk'])) {
    $settings['noBulk'] = 0;
}
$is_ssl = isSslConnection();
$GLOBALS['cdnPath'] = '';
if (isset($settings['cdn']) && !$is_ssl) {
    $GLOBALS['cdnPath'] = $settings['cdn'];
}

$tz_offset = null;
if (isset($_COOKIE['tzo'])) {
    $tz_offset = (int)$_COOKIE['tzo'];
}
SetLocaleFromBrowser();

// some myBB integration to get the requesting user
$supportsAuth = false;
if (GetSetting('google_oauth_client_id') && GetSetting('google_oauth_client_secret'))
  $supportsAuth = true;
$uid = NULL;
$user = NULL;
$admin = false;
$api_keys;
if (is_dir('./forums') && (!array_key_exists('disableMybb', $settings) || !$settings['disableMybb'])) {
  ob_start();
  $supportsAuth = true;

  if (isset($_COOKIE['mybbuser'])) {
    if (function_exists("apcu_fetch")) {
      $userinfo = apcu_fetch("mybb-user-{$_COOKIE['mybbuser']}");
      if (isset($userinfo) &&
          is_array($userinfo) &&
          isset($userinfo['uid']) &&
          isset($userinfo['user']) &&
          isset($userinfo['admin'])) {
        $uid = $userinfo['uid'];
        $user = $userinfo['user'];
        $admin = $userinfo['admin'];
      }
    }
    if (!isset($user)) {
      $dir = getcwd();
      try {
          define("IN_MYBB",1);
          chdir('forums'); // path to MyBB
          include './global.php';

          $uid = $mybb->user['uid'];
          $user = $mybb->user['username'];
          if( $mybb->usergroup['cancp'] )
              $admin = true;
          unset($mybb);
      }
      catch(Exception $e) {
      }
      chdir($dir);
      if (function_exists('apcu_store')) {
        $userinfo = array('uid' => $uid, 'user' => $user, 'admin' => $admin);
        apcu_store("mybb-user-{$_COOKIE['mybbuser']}", $userinfo, 3600);
      }
    }
  }
  ob_end_clean();
}

if (!$admin) {
  $this_user = null;
  if (isset($user)) {
    $this_user = $user;
  } elseif( isset($_COOKIE['google_email']) && isset($_COOKIE['google_id']) ) {
    $this_user = $_COOKIE['google_email'];
  }
  if (isset($this_user) && strlen($this_user)) {
    $admin_users = GetSetting("admin_users");
    if ($admin_users) {
      $admin_users = explode(',', $admin_users);
      if (is_array($admin_users) && count($admin_users)) {
        foreach($admin_users as $substr) {
          if (stripos($this_user, $substr) !== false) {
            $admin = true;
            break;
          }
        }
      }
    }
  }
}

// assign a unique ID to each person
$isFirstVisit = true;
$isOwner = false;
$owner = null;
if( isset($_COOKIE['google_id']) && strlen($_COOKIE['google_id']) ) {
  $isFirstVisit = false;
  $owner = $_COOKIE['google_id'];
} else if( isset($_COOKIE['o']) && strlen($_COOKIE['o']) ){
  $isFirstVisit = false;
  $owner = $_COOKIE['o'];
}else{
  $owner = sha1(uniqid(uniqid('', true), true));
}
setcookie('o', $owner, time()+60*60*24*365, '/');

if ($admin) {
    $settings['enable_google_csi'] = true;
}

// set their color selection as a cookie
if (isset($_REQUEST['color'])) {
    setcookie('color', $_REQUEST['color'], time()+60*60*24*365, '/');
    $_REQUEST['color'] = $_REQUEST['color'];
}

// Load the test-specific data
$id = '';
if (preg_match('/^[a-zA-Z0-9_]+$/', @$_REQUEST['test'])) {
    $id = $_REQUEST['test'];
} elseif (preg_match('/^[a-zA-Z0-9_]+$/', @$_COOKIE['tid'])) {
    $id = $_COOKIE['tid'];
}

$median_metric = isset($settings['medianMetric']) ? $settings['medianMetric'] : 'loadTime';
$testLabel = '';
if (strlen($id)) {
    if(extension_loaded('newrelic')) {
        newrelic_add_custom_parameter ('testID', $id);
    }  
    if (@strlen($_REQUEST['rkey'])) {
        // We are serving a relay request, so munge the id accordingly.
        $id = trim(htmlspecialchars($_REQUEST['rkey'])) . ".$id";
    }
    if ($id !== @$_COOKIE['tid']) {
        setcookie('tid', $id);  // update session cookie
    }
    if (!$userIsBot)
      RestoreTest($id);   // restore the test if it is archived (and deleted)

    $testPath = './' . GetTestPath($id);
    $test = array();
    if (is_file("$testPath/testinfo.ini")) {
        $test = @parse_ini_file("$testPath/testinfo.ini",true);
        if (!$userIsBot)
          touch("$testPath/testinfo.ini");
    }
    $test['testinfo'] = GetTestInfo($id);
    if (isset($test['testinfo']['medianMetric']))
      $median_metric = $test['testinfo']['medianMetric'];

    $run = isset($_REQUEST['run']) ? intval($_REQUEST['run']) : 0;
    if (!$run) {
        $run = (int)1;
    }
    $step = max(1, @(int)$_REQUEST['step']); // default is step=1
    $cached = @(int)$_REQUEST['cached'];  // define a global used in other files
    if (array_key_exists('run', $_REQUEST) && !strcasecmp($_REQUEST['run'], 'median')) {
      require_once 'include/TestInfo.php';
      require_once 'include/TestResults.php';
      $testInfo = TestInfo::fromFiles($testPath);
      $testResults = TestResults::fromFiles($testInfo);
      $run = $testResults->getMedianRunNumber($median_metric, $cached);
    }
    $cachedText = $cached ? '_Cached' : '';
    $testDate = null;
    if ($test['testinfo']) {
      if( array_key_exists('completed', $test['testinfo']))
          $testDate = strftime('%x %X', (int)$test['testinfo']['completed'] + ($tz_offset * 60));
      if (array_key_exists('owner', $test['testinfo']) && strlen($owner) && $owner == $test['testinfo']['owner'])
          $isOwner = true;
      elseif (array_key_exists('uid', $test['testinfo']) && strlen($uid) && $uid == $test['testinfo']['uid'])
          $isOwner = true;

      $url = array_key_exists('url', $test['testinfo']) ? htmlspecialchars($test['testinfo']['url']) : null;
      $dom = array_key_exists('domElement', $test['testinfo']) ? htmlspecialchars($test['testinfo']['domElement']) : null;
      $login = array_key_exists('login', $test['testinfo']) ? htmlspecialchars($test['testinfo']['login']) : null;
      $blockString = array_key_exists('block', $test['testinfo']) ? htmlspecialchars($test['testinfo']['block']) : null;
      $label = array_key_exists('label', $test['testinfo']) ? htmlspecialchars($test['testinfo']['label']) : null;
    }

    // build a common label to add to the title of each of the results pages
    if (isset($test["test"]) && isset($test["test"]["location"])) {
        $locs = preg_replace('/<.*>/U', '', $test["test"]["location"]);
        $locscitypos =  strpos($locs, ",");
        if ($locscitypos)
            $locs = substr($locs,0,strpos($locs, ","));
        $url_temp = $url;
        if (substr($url,0,7) == 'http://')
            $url_temp = substr($url,7);
        elseif (substr($url,0,8) == 'https://')
            $url_temp = substr($url,8);
        if ($label)
            $label = $label . " : ";
        $testLabel = FitText(' - ' . $locs . ' : ' . $label . $url_temp, 40);
        if( isset($testDate) && strlen($testDate) )
            $testLabel .= " - $testDate";
    }

    if (!isset($test)) {
        $test = array();
    }

    if (!array_key_exists('testinfo', $test)) {
        $test['testinfo'] = array();
    }
}

if (array_key_exists('medianMetric', $_REQUEST)) {
    $median_metric = htmlspecialchars($_REQUEST['medianMetric']);
}

if (is_file('./settings/custom_common.inc.php')) {
    include('./settings/custom_common.inc.php');
}
