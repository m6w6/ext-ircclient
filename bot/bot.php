<?php

namespace irc\client;

/* PHP-5.3 */
function origin($origin, $key) {
	$origin = parse_origin($origin);
	return $origin[$key];
}

class Robot extends Session
{
	protected $config;
	protected $connected = false;
	protected $joined = array();
	protected $work = array();
	
	function __construct($config) {
		$this->configure($config);
		parent::__construct($this->config->nick, $this->config->user, $this->config->real);
	}
	
	function run($watch_stdin = false) {
		printf("Connecting to %s...\n", $this->config->host);
		$this->doConnect($this->config->ipv6, $this->config->host, $this->config->port ?: 6667);
		
		if ($watch_stdin) {
			for (	stream_set_blocking(STDIN, 0), $i = 0, $x = ["–","\\","|","/"];
					false !== ($fds = @parent::run(array(STDIN), null, 1));
					++$i) {
				if (!$this->isConnected()) {
					printf("  %s \r", $x[$i%4]);
				}
				if ($fds[0]) {
					switch ($command = fgets(STDIN)) {
					case "quit\n":
						$this->disconnect();
						break 2;
					case "reload\n":
						$this->reload();
						break;
					case "update\n":
						$this->update();
						break;
					default:
						$this->doRaw($command);
					}
				} else {
					$this->work();
				}
			}
		} else {
			while (false !== parent::run(null, null, 1)) {
				$this->work();
			}
		}
		printf("Bye!\n");
	}
	
	function isConnected() {
		return $this->connected;
	}
	
	function disconnect() {
		$this->connected = false;
		parent::disconnect();
	}
	
	function configure($config) {
		$this->config = (object) (parse_ini_file($config, true) + ["config" => $config]);
	}
	
	function reload() {
		printf("Reloading config...\n");
		$this->configure($this->config->config);
		$this->join();
	}
	
	function update() {
		foreach ($this->joined as $channel) {
			$this->doNames($channel);
		}
	}
	
	function eventName($event) {
		static $map;
		
		if (empty($map)) {
			$rfl = new \ReflectionExtension("ircclient");
			$map = array_map(function($v) {
				return substr($v, 11);
			}, array_flip(
				$rfl->getConstants()
			));
		}
		
		return isset($map[$event]) ? $map[$event] : "UNKNOWN_$event";
	}
	
	function work() {
		if (!empty($this->work)) {
			printf("Checking work queue (%d)\n", $this->eventName($event), count($this->work));
			list($cb_func, $cb_args) = array_shift($this->work);
			call_user_func_array($cb_func, $cb_args);
		}
	}
	
	function onError($origin, array $args) {
		fprintf(STDERR, "ERROR: %s %s\n", $origin, implode(" ", $args));
	}
	
	function onNumeric($origin, $event, array $args) {
		static $buf = array();

		switch($event) {
		case RPL_NAMREPLY:
			if ((list(,, $channel, $users) = $args)) {
				$buf["names"][$channel] = array_merge((array) $buf["names"][$channel], explode(" ", $users));
			}
			break;
			
		case RPL_ENDOFNAMES:
			if ((list(, $channel) = $args)) {
				foreach ($buf["names"][$channel] as $user) {
					if ($user{0} === "@") {
						$user = substr($user, 1);
					}
					if ($user !== $this->config->nick) {
						printf("Adding work: WHOIS %s\n", $user);
						$this->work[] = [
							[$this, "doWhois"],
							[$user]
						];
					}
				}
			}
			$buf["names"][$channel] = array();
			break;
			
		case RPL_WHOISUSER:
			if ((list(, $nick, $user, $host, , $real) = $args)) {
				$this->op(substr($user, 1) ."@" . $host);
			}
			break;
		}
		
		fprintf(STDERR, "DEBUG: %s %s %s\n", $origin, $this->eventName($event), "<".implode("> <", $args).">");
	}
	
	function onConnect($origin, array $args) {
		list($nick) = $args;
		$this->connected = true;
		printf("Connected to %s as %s\n", $origin, $nick);
		$this->join();
	}
	
	function join() {
		$nopart = array();
		foreach (array_filter((array) $this->config, "is_array") as $channel => $config) {
			$nopart[] = $channel;
			printf("Joining %s...\n", $channel);
			$this->doJoin($channel, strlen($config["pass"]) ? $config["pass"] : NULL);
		}
		foreach (array_diff($this->joined, $nopart) as $channel) {
			printf("Leaving %s...\n", $channel);
			$this->doPart($channel);
		}
	}
	
	function op($channel, $origin) {
		if (preg_match($this->config->{$channel}["oper"], $origin)) {
			$nick = origin($origin, "nick");
			printf("Set +o %s\n", $nick);
			$this->doChannelMode($channel, "+o $nick");
		}
	}
	
	function onPart($origin, array $args) {
		$nick = origin($origin, "nick");
		
		if ($nick == $this->config->nick && (list($channel) = $args)) {
			printf("Left %s\n", $channel);
			unset($this->joined[array_search($this->joined, $channel, true)]);
		}
	}

	function onJoin($origin, array $args) {
		list($channel) = $args;
		$nick = origin($origin, "nick");

		if ($nick === $this->config->nick) {
			printf("Joined %s\n", $channel);
			$this->joined[] = $channel;
		} else {
			printf("%s joined %s\n", $origin, $channel);
			$this->op($channel, $origin);
		}
	}
	
	function onMode($origin, array $args) {
		if (count($args) >= 3) {
			list($channel, $mode, $users) = $args;

			if ($mode === "+o") {
				printf("Got +o %s\n", $users);
				
				if (preg_match(sprintf("/\b%s\b/", preg_quote($this->config->nick)), $users)) {
					$this->doNames($channel);
				}
			}
		}
	}
}
