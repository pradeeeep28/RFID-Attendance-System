<?php
    $uid = $_POST["uid"];
    $Write = "<?php $" . "uid='" . $uid . "'; " . "echo $" . "uid;" . " ?>";
    file_put_contents('UIDContainer.php', $Write);
?>