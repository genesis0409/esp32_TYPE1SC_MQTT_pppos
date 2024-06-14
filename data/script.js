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

// select 태그의 값이 변경될 때마다 input 태그의 required 속성을 조정
// 문서가 준비되면 실행되는 함수
$(function () {
  // select 요소의 값에 따라 input 요소의 required 속성을 토글하는 함수
  function toggleRequiredAttribute(sensorId, slaveId) {
    var selectedSensor = $(sensorId).val();
    var $input = $(slaveId);

    if (selectedSensor === "sensorId_none") {
      $input.removeAttr("required");
    } else {
      $input.attr("required", "required");
    }
  }

  function setupSensorAndSlave(sensorId, slaveId) {
    // select 요소에 change 이벤트 리스너를 추가하고 초기 상태 설정
    $(sensorId).change(function () {
      toggleRequiredAttribute(sensorId, slaveId);
    });

    // 초기 로드 시에도 select 요소의 값에 따라 required 속성을 설정
    toggleRequiredAttribute(sensorId, slaveId);
  }

  // Sensor 01과 Slave ID 01에 대한 설정
  setupSensorAndSlave("#sensorId_01_short", "#slaveId_01_short");

  // Sensor 02와 Slave ID 02에 대한 설정
  setupSensorAndSlave("#sensorId_02_short", "#slaveId_02_short");
});

// // sensorId_01
// $(function () {
//   $("#sensorId_01_short").change(function () {
//     var selectValue = $(this).val();
//     var $input = $("#slaveId_01_short");

//     if (selectValue === "sensorId_none") {
//       $input.removeAttr("required");
//     } else {
//       $input.attr("required", "required");
//     }
//   });
// });

// // sensorId_02
// $(function () {
//   $("#sensorId_02_short").change(function () {
//     var selectValue = $(this).val();
//     var $input = $("#slaveId_02_short");

//     if (selectValue === "sensorId_none") {
//       $input.removeAttr("required");
//     } else {
//       $input.attr("required", "required");
//     }
//   });
// });
