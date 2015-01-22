#!/usr/bin/php
<?php
echo "Content-Type: text/html\r\n";
echo "\r\n";

echo "Hello，{$_SERVER['REQUEST_METHOD']}";