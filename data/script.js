// 패스워드 visible/invisible
$(function () {
  $("#toggle-password").on("click", function () {
    let passwordField = $("#mqttPw");
    let passwordFieldType = passwordField.attr("type");
    let toggleIcon = $("#toggle-password");

    if (passwordFieldType === "password") {
      passwordField.attr("type", "text");
      toggleIcon.attr("src", "eye-slash.svg");
    } else {
      passwordField.attr("type", "password");
      toggleIcon.attr("src", "eye.svg");
    }
  });
});

// comboBox 중복 선택 배제
$(function () {
  $("#sensorId_01_short").change(function () {
    updateSelects();
  });

  $("#sensorId_02_short").change(function () {
    updateSelects();
  });

  function updateSelects() {
    var selectedValue1 = $("#sensorId_01_short").val();
    var selectedValue2 = $("#sensorId_02_short").val();

    // 모든 option 태그 활성화
    $("#sensorId_01_short option").prop("disabled", false);
    $("#sensorId_02_short option").prop("disabled", false);

    // id가 "boundaryLine"인 옵션을 비활성화; 선택할 일이 없으므로 refresh할 필요 없는 코드 위치
    $('#sensorId_01_short option[id = "boundaryLine"]').prop("disabled", true);
    $('#sensorId_02_short option[id = "boundaryLine"]').prop("disabled", true);

    if (selectedValue1) {
      $('#sensorId_02_short option[value="' + selectedValue1 + '"]').prop(
        "disabled",
        true
      );

      // 추가 기능 ******************************************************
      // value가 "sensorId_none"인 옵션을 활성화
      $('#sensorId_01_short option[value="sensorId_none"]').prop(
        "disabled",
        false
      );
      $('#sensorId_02_short option[value="sensorId_none"]').prop(
        "disabled",
        false
      );

      // value가 "sensorId_ec", "sensorId_soil"인 옵션을 비활성화
      $('#sensorId_01_short option[value="sensorId_ec"]').prop(
        "disabled",
        true
      );
      $('#sensorId_02_short option[value="sensorId_ec"]').prop(
        "disabled",
        true
      );
      $('#sensorId_01_short option[value="sensorId_soil"]').prop(
        "disabled",
        true
      );
      $('#sensorId_02_short option[value="sensorId_soil"]').prop(
        "disabled",
        true
      );
      // ***************************************************************
    }

    if (selectedValue2) {
      $('#sensorId_01_short option[value="' + selectedValue2 + '"]').prop(
        "disabled",
        true
      );

      // 추가 기능 ******************************************************
      // value가 "sensorId_none"인 옵션을 활성화
      $('#sensorId_01_short option[value="sensorId_none"]').prop(
        "disabled",
        false
      );
      $('#sensorId_02_short option[value="sensorId_none"]').prop(
        "disabled",
        false
      );

      // value가 "sensorId_ec", "sensorId_soil"인 옵션을 비활성화
      $('#sensorId_01_short option[value="sensorId_ec"]').prop(
        "disabled",
        true
      );
      $('#sensorId_02_short option[value="sensorId_ec"]').prop(
        "disabled",
        true
      );
      $('#sensorId_01_short option[value="sensorId_soil"]').prop(
        "disabled",
        true
      );
      $('#sensorId_02_short option[value="sensorId_soil"]').prop(
        "disabled",
        true
      );
      // ***************************************************************
    }
  }

  // 페이지 로드 시 초기화
  updateSelects();
});
